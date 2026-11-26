#include "processors/lot_carry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "taxbroker/exact_arithmetic.hpp"

namespace taxbroker::lot_carry {

std::size_t instrumentIndexOf(const MergedStatement& aStatement, const Isin& aIsin) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    const auto found = std::find_if(
        instruments.begin(),
        instruments.end(),
        [&aIsin](const TradeInstrument& aInstrument) { return aInstrument.mIsin == aIsin; });

    return static_cast<std::size_t>(found - instruments.begin());
}

bool hasSaleInYear(const TradeInstrument& aInstrument,
                   const ReportingYearLedger& aLedger,
                   const std::vector<std::size_t>& aOwnRows) {
    const auto inYear = [&aLedger](Date aDate) {
        return aDate >= aLedger.mYearStart && aDate <= aLedger.mYearEnd;
    };

    const bool hasSale = std::any_of(aInstrument.mTransactions.begin(),
                                     aInstrument.mTransactions.end(),
                                     [&inYear](const TradeTransaction& aTrade) {
                                         return aTrade.mTradeSide == TradeSide::Sell &&
                                                inYear(aTrade.mMetadata.mTaxDate);
                                     });

    if (hasSale)
    {
        return true;
    }

    // Units that leave in another change of the year are a sale once that change is answered.
    for (std::size_t row = 0; row < aInstrument.mCorporateActions.size(); ++row)
    {
        const CorporateAction& action = aInstrument.mCorporateActions[row];
        const bool isChange = action.mType == CorporateActionType::IsinChange ||
                              action.mType == CorporateActionType::StockMerger ||
                              action.mType == CorporateActionType::StockMergerPending;

        if (isChange && action.mUnitsDelta < 0 && inYear(action.mMetadata.mTaxDate) &&
            std::find(aOwnRows.begin(), aOwnRows.end(), row) == aOwnRows.end())
        {
            return true;
        }
    }

    return false;
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

void carryOut(MergedStatement& aStatement,
              const IsinChangePair& aPair,
              const IsinChangeAnswer& aAnswer,
              const bool aMarkPurchasesAsExchange) {
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

        if (aMarkPurchasesAsExchange && trade.mTradeSide == TradeSide::Buy)
        {
            trade.mPurchaseKind = PurchaseKind::StatusChangeExchange;
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

} // namespace taxbroker::lot_carry
