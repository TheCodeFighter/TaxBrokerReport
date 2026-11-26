#include "processors/isin_change.hpp"
#include "processors/lot_carry.hpp"
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

using namespace lot_carry;

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

        // An ISIN change matters when a sale of the year is of either ISIN (also one that a later
        // change of the year makes) and it is not after the end of the loss-analysis view.
        const bool matters = (hasSaleInYear(oldInstrument, aLedger, pair.mOutRows) ||
                              hasSaleInYear(newInstrument, aLedger, pair.mInRows)) &&
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
        carryOut(writable(), pair, *change, false);
    }

    result.mStatement = std::move(changed);

    return result;
}

} // namespace taxbroker
