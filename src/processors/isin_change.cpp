#include "processors/isin_change.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/text_format.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace taxbroker {
namespace {

/// The two ISINs of one ISIN change and where their rows are, by ISIN (positions move when a
/// change is carried out).
struct IsinChangePair {
    Isin mOldIsin;
    Isin mNewIsin;
    std::vector<std::size_t> mOutRows; ///< Rows of the old instrument that lose units.
    std::vector<std::size_t> mInRows;  ///< Rows of the new instrument that get them.
};

std::size_t instrumentIndexOf(const MergedStatement& aStatement, const Isin& aIsin) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    const auto found = std::find_if(
        instruments.begin(),
        instruments.end(),
        [&aIsin](const TradeInstrument& aInstrument) { return aInstrument.mIsin == aIsin; });

    return static_cast<std::size_t>(found - instruments.begin());
}

bool hasSaleInYear(const TradeInstrument& aInstrument, const ReportingYearLedger& aLedger) {
    return std::any_of(aInstrument.mTransactions.begin(),
                       aInstrument.mTransactions.end(),
                       [&aLedger](const TradeTransaction& aTrade) {
                           const auto date = aTrade.mMetadata.mTaxDate;

                           return aTrade.mTradeSide == TradeSide::Sell &&
                                  date >= aLedger.mYearStart && date <= aLedger.mYearEnd;
                       });
}

/// The ISIN changes of the statement, one for each old ISIN and new ISIN: the rows of the old
/// instrument that name the new ISIN, and the rows of the new instrument of the same action.
/// Rows of one action are matched by the broker's action ID, or by date when there is none.
std::vector<IsinChangePair> pairsOf(const MergedStatement& aStatement) {
    std::vector<IsinChangePair> pairs;
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;

    for (const auto& instrument : instruments)
    {
        for (std::size_t row = 0; row < instrument.mCorporateActions.size(); ++row)
        {
            const auto& action = instrument.mCorporateActions[row];

            if (action.mType != CorporateActionType::IsinChange || !action.mNewIsin)
            {
                continue;
            }

            auto pair = std::find_if(pairs.begin(),
                                     pairs.end(),
                                     [&instrument, &action](const IsinChangePair& aPair) {
                                         return aPair.mOldIsin == instrument.mIsin &&
                                                aPair.mNewIsin == *action.mNewIsin;
                                     });

            if (pair == pairs.end())
            {
                pairs.push_back({.mOldIsin = instrument.mIsin, .mNewIsin = *action.mNewIsin});
                pair = pairs.end() - 1;
            }

            pair->mOutRows.push_back(row);
        }
    }

    for (auto& pair : pairs)
    {
        const auto newPosition = instrumentIndexOf(aStatement, pair.mNewIsin);

        if (newPosition == instruments.size())
        {
            continue;
        }

        const auto& oldInstrument = instruments[instrumentIndexOf(aStatement, pair.mOldIsin)];
        std::set<std::string> actionIds;
        bool hasRowWithoutId = false;

        for (const auto row : pair.mOutRows)
        {
            const auto& id = oldInstrument.mCorporateActions[row].mBrokerActionId;

            if (id)
            {
                actionIds.insert(*id);
            }
            else
            {
                hasRowWithoutId = true;
            }
        }

        const auto& newActions = instruments[newPosition].mCorporateActions;
        const auto firstDate =
            oldInstrument.mCorporateActions[pair.mOutRows.front()].mMetadata.mTaxDate;

        for (std::size_t row = 0; row < newActions.size(); ++row)
        {
            const auto& action = newActions[row];

            if (action.mType != CorporateActionType::IsinChange || action.mNewIsin)
            {
                continue;
            }

            const auto date = action.mMetadata.mTaxDate;
            const auto distance = date < firstDate ? firstDate - date : date - firstDate;
            const bool sameId =
                action.mBrokerActionId && actionIds.contains(*action.mBrokerActionId);
            const bool sameDay = !action.mBrokerActionId && hasRowWithoutId &&
                                 distance <= DayDuration{kSameActionWindowDays};

            if (sameId || sameDay)
            {
                pair.mInRows.push_back(row);
            }
        }
    }

    return pairs;
}

/// The total of the units of the rows, with the sign of the units that left: positive for the
/// rows of one side. None when the rows have no units on that side or the total does not fit.
std::optional<Units>
totalUnits(const TradeInstrument& aInstrument, const std::vector<std::size_t>& aRows, bool aLeft) {
    Units total = 0;

    for (const auto row : aRows)
    {
        const auto sum = checkedAdd(total, aInstrument.mCorporateActions[row].mUnitsDelta);

        if (const auto* value = std::get_if<std::int64_t>(&sum))
        {
            total = *value;
        }
        else
        {
            return std::nullopt;
        }
    }

    if (aLeft)
    {
        // The units that left are negative; the minimum has no positive counterpart.
        return total < 0 && total != std::numeric_limits<Units>::min()
                   ? std::optional<Units>{-total}
                   : std::nullopt;
    }

    return total > 0 ? std::optional<Units>{total} : std::nullopt;
}

/// The ratio the rows show: the units that came in over the units that left, reduced. None when
/// the rows do not give both totals.
std::optional<SplitRatio>
ratioOfRows(const TradeInstrument& aOld, const TradeInstrument& aNew, const IsinChangePair& aPair) {
    const auto left = totalUnits(aOld, aPair.mOutRows, true);
    const auto came = totalUnits(aNew, aPair.mInRows, false);

    if (!left || !came)
    {
        return std::nullopt;
    }

    const auto divisor = std::gcd(*left, *came);

    return SplitRatio{.mNewShares = *came / divisor, .mOldShares = *left / divisor};
}

/// The facts of one action row for the question.
CorporateActionSourceFacts factsOf(const CorporateAction& aAction) {
    CorporateActionSourceFacts facts{
        .mBroker = aAction.mMetadata.mSources.empty() ? Broker::Unknown
                                                      : aAction.mMetadata.mSources.front().mBroker,
        .mDate = aAction.mMetadata.mTaxDate,
    };

    if (const auto identity = transactionIdentity(aAction.mMetadata))
    {
        facts.mTransactionId = identity->mTransactionId;
    }

    // Copies of one record share the value, so the first evidence is enough.
    if (!aAction.mUnitEvidence.empty())
    {
        facts.mSourceValue = aAction.mUnitEvidence.front().mCanonicalValue;
    }

    return facts;
}

/// The diagnostic of one of the two ISINs of the change, with the sources of every row.
ProcessingDiagnostic isinDiagnostic(DiagnosticSeverity aSeverity,
                                    ProcessingDiagnosticCode aCode,
                                    DiagnosticMessage aMessage,
                                    const TradeInstrument& aInstrument,
                                    const std::vector<SourceReference>& aSources,
                                    Date aDate) {
    return ProcessingDiagnostic{
        .mSeverity = aSeverity,
        .mCode = aCode,
        .mContext =
            {
                .mStage = PipelineStage::CorporateAction,
                .mAffectedReports = {ReportType::CapitalGains},
                .mSources = aSources,
                .mTaxDate = aDate,
                .mInstrumentName = aInstrument.mName,
                .mIsin = aInstrument.mIsin,
            },
        .mMessage = std::move(aMessage.mEnglish),
        .mMessageKey = aMessage.mKey,
        .mMessageParams = std::move(aMessage.mParams),
    };
}

/// Tells whether a trade or action row of the old instrument is on or after aDate, or one of the
/// new instrument before it, apart from the rows of the change itself.
bool crossesDate(const TradeInstrument& aOld,
                 const TradeInstrument& aNew,
                 const IsinChangePair& aPair,
                 Date aDate) {
    const auto isRowOfChange = [](const std::vector<std::size_t>& aRows, std::size_t aRow) {
        return std::find(aRows.begin(), aRows.end(), aRow) != aRows.end();
    };

    const bool oldTradeLate = std::any_of(
        aOld.mTransactions.begin(),
        aOld.mTransactions.end(),
        [aDate](const TradeTransaction& aTrade) { return aTrade.mMetadata.mTaxDate >= aDate; });
    const bool newTradeEarly = std::any_of(
        aNew.mTransactions.begin(),
        aNew.mTransactions.end(),
        [aDate](const TradeTransaction& aTrade) { return aTrade.mMetadata.mTaxDate < aDate; });

    bool oldActionLate = false;

    for (std::size_t row = 0; row < aOld.mCorporateActions.size(); ++row)
    {
        oldActionLate = oldActionLate || (!isRowOfChange(aPair.mOutRows, row) &&
                                          aOld.mCorporateActions[row].mMetadata.mTaxDate >= aDate);
    }

    bool newActionEarly = false;

    for (std::size_t row = 0; row < aNew.mCorporateActions.size(); ++row)
    {
        newActionEarly = newActionEarly || (!isRowOfChange(aPair.mInRows, row) &&
                                            aNew.mCorporateActions[row].mMetadata.mTaxDate < aDate);
    }

    return oldTradeLate || newTradeEarly || oldActionLate || newActionEarly;
}

template <typename Event> void sortChronologically(std::vector<Event>& aEvents) {
    std::stable_sort(aEvents.begin(), aEvents.end(), [](const Event& aLeft, const Event& aRight) {
        return ChronologicalEventOrder{}(aLeft.mMetadata, aRight.mMetadata);
    });
}

/// Moves the lots of the old instrument to the new one. The old instrument is removed. The
/// rows of the change become one action row of the new ISIN with the ratio confirmed, or none
/// when the number of units does not change.
void carryOut(MergedStatement& aStatement,
              const IsinChangePair& aPair,
              const IsinChangeAnswer& aAnswer) {
    auto& instruments = aStatement.mPresentation.mTradeInstruments;
    const auto oldPosition = instrumentIndexOf(aStatement, aPair.mOldIsin);
    const auto newPosition = instrumentIndexOf(aStatement, aPair.mNewIsin);
    auto oldInstrument = std::move(instruments[oldPosition]);
    auto& newInstrument = instruments[newPosition];

    // The row of the new ISIN that stays keeps the sources of every row of the action. It is moved
    // out here and its old place is erased below.
    auto kept = std::move(newInstrument.mCorporateActions[aPair.mInRows.front()]);

    for (const auto row : aPair.mInRows)
    {
        if (row == aPair.mInRows.front())
        {
            continue;
        }

        const auto& other = newInstrument.mCorporateActions[row];

        kept.mMetadata.mSources.insert(kept.mMetadata.mSources.end(),
                                       other.mMetadata.mSources.begin(),
                                       other.mMetadata.mSources.end());
        kept.mUnitEvidence.insert(kept.mUnitEvidence.end(),
                                  other.mUnitEvidence.begin(),
                                  other.mUnitEvidence.end());
    }

    for (const auto row : aPair.mOutRows)
    {
        const auto& out = oldInstrument.mCorporateActions[row];

        kept.mMetadata.mSources.insert(kept.mMetadata.mSources.end(),
                                       out.mMetadata.mSources.begin(),
                                       out.mMetadata.mSources.end());
    }

    // Erase from the back, so the earlier positions stay valid. The rows of a pair are in
    // ascending order (pairsOf walks the actions in order).
    for (auto row = aPair.mInRows.rbegin(); row != aPair.mInRows.rend(); ++row)
    {
        newInstrument.mCorporateActions.erase(newInstrument.mCorporateActions.begin() +
                                              static_cast<std::ptrdiff_t>(*row));
    }

    for (auto row = aPair.mOutRows.rbegin(); row != aPair.mOutRows.rend(); ++row)
    {
        oldInstrument.mCorporateActions.erase(oldInstrument.mCorporateActions.begin() +
                                              static_cast<std::ptrdiff_t>(*row));
    }

    if (aAnswer.mNewShares != aAnswer.mOldShares)
    {
        // The position of the ratio is the confirmed one; the row is applied like a confirmed
        // split. An effective date other than the row's has no time of day.
        if (kept.mMetadata.mTaxDate != aAnswer.mEffectiveDate)
        {
            kept.mMetadata.mTaxDate = aAnswer.mEffectiveDate;
            kept.mMetadata.mSourceTimestamp = std::nullopt;
            kept.mMetadata.mOrderingTimestamp = std::nullopt;
        }

        kept.mType = aAnswer.mNewShares > aAnswer.mOldShares ? CorporateActionType::Split
                                                             : CorporateActionType::ReverseSplit;
        kept.mRatio =
            SplitRatio{.mNewShares = aAnswer.mNewShares, .mOldShares = aAnswer.mOldShares};
        kept.mNewIsin = std::nullopt;
        newInstrument.mCorporateActions.push_back(std::move(kept));
    }

    // A sale stays listed under the ISIN and name it had on its date; a trade that an earlier
    // change moved keeps the first one.
    for (auto& trade : oldInstrument.mTransactions)
    {
        if (!trade.mListedAs)
        {
            trade.mListedAs =
                ListedInstrument{.mIsin = oldInstrument.mIsin, .mName = oldInstrument.mName};
        }
    }

    newInstrument.mTransactions.insert(newInstrument.mTransactions.end(),
                                       std::make_move_iterator(oldInstrument.mTransactions.begin()),
                                       std::make_move_iterator(oldInstrument.mTransactions.end()));
    newInstrument.mCorporateActions.insert(
        newInstrument.mCorporateActions.end(),
        std::make_move_iterator(oldInstrument.mCorporateActions.begin()),
        std::make_move_iterator(oldInstrument.mCorporateActions.end()));
    sortChronologically(newInstrument.mTransactions);
    sortChronologically(newInstrument.mCorporateActions);

    if (newInstrument.mAssetClass == AssetClass::Unknown)
    {
        newInstrument.mAssetClass = oldInstrument.mAssetClass;
    }

    if (newInstrument.mName.empty())
    {
        newInstrument.mName = oldInstrument.mName;
    }

    instruments.erase(instruments.begin() + static_cast<std::ptrdiff_t>(oldPosition));
    rebuildChronologicalOrder(aStatement);
}

void retype(MergedStatement& aStatement, const IsinChangePair& aPair, CorporateActionType aType) {
    auto& instruments = aStatement.mPresentation.mTradeInstruments;

    for (const auto row : aPair.mOutRows)
    {
        instruments[instrumentIndexOf(aStatement, aPair.mOldIsin)].mCorporateActions[row].mType =
            aType;
    }

    for (const auto row : aPair.mInRows)
    {
        instruments[instrumentIndexOf(aStatement, aPair.mNewIsin)].mCorporateActions[row].mType =
            aType;
    }
}

} // namespace

IsinChangeResult applyIsinChanges(const MergedStatement& aStatement,
                                  const ReportingYearLedger& aLedger,
                                  RunDecisions& aDecisions) {
    IsinChangeResult result;
    auto pairs = pairsOf(aStatement);

    // Most runs have no ISIN change to carry out: the statement is read in place and copied only
    // when the first row has to change.
    if (pairs.empty())
    {
        return result;
    }

    std::optional<MergedStatement> changed;
    const auto current = [&aStatement, &changed]() -> const MergedStatement& {
        return changed ? *changed : aStatement;
    };
    const auto writable = [&aStatement, &changed]() -> MergedStatement& {
        if (!changed)
        {
            changed = aStatement;
        }

        return *changed;
    };

    // A row that names a new ISIN without a partner is an action the app does not know.
    for (const auto& pair : pairs)
    {
        if (pair.mInRows.empty())
        {
            retype(writable(), pair, CorporateActionType::Merger);
        }
    }

    std::erase_if(pairs, [](const IsinChangePair& aPair) { return aPair.mInRows.empty(); });

    // One pair after the other in date order; each carried out change moves the positions, so
    // the rows are found again by ISIN.
    std::stable_sort(pairs.begin(),
                     pairs.end(),
                     [&aStatement](const IsinChangePair& aLeft, const IsinChangePair& aRight) {
                         const auto date = [&aStatement](const IsinChangePair& aPair) {
                             return aStatement.mPresentation
                                 .mTradeInstruments[instrumentIndexOf(aStatement, aPair.mOldIsin)]
                                 .mCorporateActions[aPair.mOutRows.front()]
                                 .mMetadata.mTaxDate;
                         };

                         return date(aLeft) < date(aRight);
                     });

    for (auto& pair : pairs)
    {
        // Rows moved only when a statement was changed: the positions are then found again.
        if (changed)
        {
            auto refreshed = pairsOf(*changed);
            const auto found = std::find_if(refreshed.begin(),
                                            refreshed.end(),
                                            [&pair](const IsinChangePair& aPair) {
                                                return aPair.mOldIsin == pair.mOldIsin &&
                                                       aPair.mNewIsin == pair.mNewIsin;
                                            });

            if (found == refreshed.end() || found->mInRows.empty())
            {
                continue;
            }

            pair = std::move(*found);
        }

        const auto& instruments = current().mPresentation.mTradeInstruments;
        const auto& oldInstrument = instruments[instrumentIndexOf(current(), pair.mOldIsin)];
        const auto& newInstrument = instruments[instrumentIndexOf(current(), pair.mNewIsin)];

        const auto& outRow = oldInstrument.mCorporateActions[pair.mOutRows.front()];
        Date earliest = outRow.mMetadata.mTaxDate;
        Date latest = earliest;
        std::vector<SourceReference> sources;
        IsinChangeQuestion data{
            .mOldIsin = pair.mOldIsin,
            .mOldName = oldInstrument.mName,
            .mNewIsin = pair.mNewIsin,
            .mNewName = newInstrument.mName,
            .mEffectiveDate = earliest,
            .mSuggestedRatio = ratioOfRows(oldInstrument, newInstrument, pair),
        };

        const auto add = [&](const CorporateAction& aAction) {
            earliest = std::min(earliest, aAction.mMetadata.mTaxDate);
            latest = std::max(latest, aAction.mMetadata.mTaxDate);
            sources.insert(sources.end(),
                           aAction.mMetadata.mSources.begin(),
                           aAction.mMetadata.mSources.end());
            data.mSources.push_back(factsOf(aAction));
        };

        for (const auto row : pair.mOutRows)
        {
            add(oldInstrument.mCorporateActions[row]);
        }

        for (const auto row : pair.mInRows)
        {
            add(newInstrument.mCorporateActions[row]);
        }

        data.mEarliestDate = earliest;
        data.mLatestDate = latest;

        // An ISIN change matters when a sale of the year is of either ISIN and it is not after
        // the end of the loss-analysis view.
        const bool matters =
            (hasSaleInYear(oldInstrument, aLedger) || hasSaleInYear(newInstrument, aLedger)) &&
            earliest <= aLedger.mLossWindowEnd;

        if (!matters)
        {
            continue;
        }

        const Date effectiveDate = data.mEffectiveDate;
        DecisionQuestion question{
            .mType = RequestType::CorporateActionIsinChangeRequired,
            .mAffectedReports = {ReportType::CapitalGains},
            .mSources = sources,
            .mTaxDate = effectiveDate,
            .mIsin = pair.mOldIsin,
            .mData = std::move(data),
        };
        const auto* answer = aDecisions.find(question);
        const auto* change = answer ? std::get_if<IsinChangeAnswer>(answer) : nullptr;

        // Both ISINs get the same diagnostic, so both stay blocked until it is resolved.
        const auto blockBoth = [&](ProcessingDiagnosticCode aCode, MessageKey aKey) {
            std::vector<std::size_t> positions;

            for (const auto* instrument : {&oldInstrument, &newInstrument})
            {
                positions.push_back(result.mDiagnostics.size());
                result.mDiagnostics.push_back(isinDiagnostic(DiagnosticSeverity::Error,
                                                             aCode,
                                                             makeDiagnosticMessage(aKey),
                                                             *instrument,
                                                             sources,
                                                             effectiveDate));
            }

            return positions;
        };

        if (answer == nullptr)
        {
            result.mRequests.push_back(
                {.mQuestion = std::move(question),
                 .mProcessingDiagnostics =
                     blockBoth(ProcessingDiagnosticCode::CorporateActionAmbiguous,
                               MessageKey::CorporateActionAmbiguousIsinChangeRequired)});
            continue;
        }

        // The session accepts only an ISIN change answer or a cancel.
        if (change == nullptr)
        {
            blockBoth(ProcessingDiagnosticCode::CorporateActionAmbiguous,
                      MessageKey::CorporateActionAmbiguousCancelled);
            continue;
        }

        if (!change->mSameIssuer)
        {
            retype(writable(), pair, CorporateActionType::StockMerger);
            continue;
        }

        if (crossesDate(oldInstrument, newInstrument, pair, change->mEffectiveDate))
        {
            blockBoth(ProcessingDiagnosticCode::CorporateActionInconsistent,
                      MessageKey::CorporateActionInconsistentTradeAcrossIsinChange);
            continue;
        }

        result.mDiagnostics.push_back(isinDiagnostic(
            DiagnosticSeverity::Warning,
            ProcessingDiagnosticCode::CorporateActionApplied,
            makeDiagnosticMessage(
                MessageKey::CorporateActionAppliedIsinChange,
                {textParam("oldIsin", pair.mOldIsin),
                 textParam("newIsin", pair.mNewIsin),
                 numberParam("newShares", static_cast<std::uint64_t>(change->mNewShares)),
                 numberParam("oldShares", static_cast<std::uint64_t>(change->mOldShares)),
                 textParam("effectiveDate", formatCalendarDate(change->mEffectiveDate))}),
            newInstrument,
            sources,
            change->mEffectiveDate));

        // Nothing below reads the instruments by reference: they may be the ones moved here.
        carryOut(writable(), pair, *change);
    }

    result.mStatement = std::move(changed);

    return result;
}

} // namespace taxbroker
