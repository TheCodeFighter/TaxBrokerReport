#include "processors/capital_gains_report_builder.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <utility>
#include <variant>

namespace taxbroker {
namespace {

std::vector<std::vector<StatementEventReference>>
collectSelectedYearSales(const std::vector<TradeInstrument>& aInstruments,
                         const ReportingYearLedger& aLedger) {
    std::vector<std::vector<StatementEventReference>> sales(aInstruments.size());

    for (std::size_t position = aLedger.mHistoricalCapitalEventCount;
         position < aLedger.mCapitalEvents.size();
         ++position)
    {
        const auto& event = aLedger.mCapitalEvents.at(position);

        if (event.mKind != StatementEventKind::Trade)
        {
            continue;
        }

        const auto instrumentIndex = event.mInstrumentIndex.value();
        const auto& trade = aInstruments.at(instrumentIndex).mTransactions.at(event.mEventIndex);

        if (trade.mTradeSide == TradeSide::Sell)
        {
            sales.at(instrumentIndex).push_back(event);
        }
    }

    return sales;
}

/// The loss-rule result of a sale, or nullptr for a sale without a loss unit.
const LossSaleEligibility* lossOf(const LossEligibilityIsin& aLoss,
                                  const StatementEventReference& aSale) {
    const auto found =
        std::find_if(aLoss.mSales.begin(),
                     aLoss.mSales.end(),
                     [&aSale](const LossSaleEligibility& aEntry) { return aEntry.mSale == aSale; });

    return found == aLoss.mSales.end() ? nullptr : &*found;
}

/// Exact EUR per unit of the matched units: the basis of the match over its units. For a lot a
/// split changed this is the basis over the units after the split.
ExactRational exactUnitValueOf(const FifoMatch& aMatch) {
    // basis ticks / (UnitPrice scale * raw units): the Units scale cancels, as in
    // exactLotUnitValue. The divisor is positive and the ticks are far below the 256-bit range.
    return std::get<ExactRational>(
        divideExact(aMatch.mBasis.mTicks,
                    ExactRational{WideInteger{UNIT_PRICE_SCALE} * aMatch.mUnits}));
}

/// The F4 value of an exact unit value, rounded once to 8 decimals, half away from zero.
NumericResult<UnitPrice> filingUnitPriceOf(const ExactRational& aUnitValue) {
    return roundToScaled(aUnitValue,
                         UNIT_PRICE_SCALE,
                         DestinationRange{.mPolicy = ValuePolicy::Nonnegative});
}

/// Tells whether the unit value of the match fits F4 (a UnitPrice).
bool hasFilingUnitPrice(const FifoMatch& aMatch) {
    return std::holds_alternative<UnitPrice>(filingUnitPriceOf(exactUnitValueOf(aMatch)));
}

/// The units of the sale that make a loss within the holding period, as the loss rule counts them.
Units listedLossUnitsOf(const FifoIsinResult& aPool, const StatementEventReference& aSale) {
    return std::accumulate(aPool.mMatches.begin(),
                           aPool.mMatches.end(),
                           Units{0},
                           [&aSale](Units aSum, const FifoMatch& aMatch) {
                               return aMatch.mSale == aSale && aMatch.mGainTicks.numerator() < 0 &&
                                              !isPastHoldingPeriod(aMatch)
                                          ? aSum + aMatch.mUnits
                                          : aSum;
                           });
}

/// Why the ISIN gets no list.
std::optional<KdvpBlockedIsin> findBlock(const FifoIsinResult& aPool,
                                         const LossEligibilityIsin& aLoss,
                                         const std::vector<StatementEventReference>& aYearSales) {
    if (aPool.mStatus != FifoIsinStatus::Matched)
    {
        return KdvpBlockedIsin{.mIsin = aPool.mIsin,
                               .mName = aPool.mName,
                               .mReason = KdvpBlockReason::FifoStopped,
                               .mSales = aYearSales};
    }

    std::vector<StatementEventReference> largeValueSales;

    // Units past the holding period get no row, so only listed units need an F4.
    for (const auto& sale : aYearSales)
    {
        const bool needsLargeValue = std::any_of(aPool.mMatches.begin(),
                                                 aPool.mMatches.end(),
                                                 [&sale](const FifoMatch& aMatch) {
                                                     return aMatch.mSale == sale &&
                                                            !isPastHoldingPeriod(aMatch) &&
                                                            !hasFilingUnitPrice(aMatch);
                                                 });

        if (needsLargeValue)
        {
            largeValueSales.push_back(sale);
        }
    }

    if (!largeValueSales.empty())
    {
        return KdvpBlockedIsin{.mIsin = aPool.mIsin,
                               .mName = aPool.mName,
                               .mReason = KdvpBlockReason::UnitValueTooLarge,
                               .mSales = std::move(largeValueSales)};
    }

    std::vector<StatementEventReference> lossSales;

    for (const auto& sale : aYearSales)
    {
        const auto* loss = lossOf(aLoss, sale);

        // A decided sale is written in one or more rows, whatever part of its loss is blocked. A
        // rule that blocks more units than the sale has loss units is a fault; the gates report it.
        if (loss != nullptr && (loss->mState != LossSaleState::Decided ||
                                loss->ineligibleUnits() > listedLossUnitsOf(aPool, sale)))
        {
            lossSales.push_back(sale);
        }
    }

    if (lossSales.empty())
    {
        return std::nullopt;
    }

    return KdvpBlockedIsin{.mIsin = aPool.mIsin,
                           .mName = aPool.mName,
                           .mReason = KdvpBlockReason::LossNotReportable,
                           .mSales = std::move(lossSales)};
}

/// Returns the tax-free units of each sale of the year, past the holding period.
std::vector<KdvpLongHeldUnits>
collectLongHeldUnits(const FifoIsinResult& aPool,
                     const std::vector<StatementEventReference>& aYearSales) {
    std::vector<KdvpLongHeldUnits> longHeld;

    for (const auto& sale : aYearSales)
    {
        const Units units =
            std::accumulate(aPool.mMatches.begin(),
                            aPool.mMatches.end(),
                            Units{0},
                            [&sale](Units aSum, const FifoMatch& aMatch) {
                                return aMatch.mSale == sale && isPastHoldingPeriod(aMatch)
                                           ? aSum + aMatch.mUnits
                                           : aSum;
                            });

        if (units > 0)
        {
            longHeld.push_back(
                {.mIsin = aPool.mIsin, .mName = aPool.mName, .mSale = sale, .mUnits = units});
        }
    }

    return longHeld;
}

/// The units of one purchase that sales of the year use, all at one exact unit value.
struct PurchaseGroup {
    std::size_t mPurchaseIndex{};
    ExactRational mUnitValue;
    Units mUnits{};
};

/// The listed units of the sales of the year, by purchase and exact unit value, in the order the
/// sales use them. A purchase sold before and after a split has one group for each unit state; a
/// sale in a different state than the last one of its purchase starts a new group.
std::vector<PurchaseGroup> purchaseGroupsOf(const FifoIsinResult& aPool) {
    std::vector<PurchaseGroup> groups;

    for (const auto& match : aPool.mMatches)
    {
        if (isPastHoldingPeriod(match))
        {
            continue;
        }

        const auto value = exactUnitValueOf(match);

        // Only the last group of the purchase can take the units: after a split and a reverse
        // split that cancel, units sold before, between and after them are three groups.
        const auto last =
            std::find_if(groups.rbegin(), groups.rend(), [&match](const PurchaseGroup& aGroup) {
                return aGroup.mPurchaseIndex == match.mPurchase.mEventIndex;
            });

        if (last != groups.rend() && last->mUnitValue == value)
        {
            last->mUnits += match.mUnits;
        }
        else
        {
            groups.push_back({.mPurchaseIndex = match.mPurchase.mEventIndex,
                              .mUnitValue = value,
                              .mUnits = match.mUnits});
        }
    }

    return groups;
}

/// A run of listed sale units with one F10 value.
struct SalePart {
    Units mUnits{};
    KdvpLossRule mLossRule{KdvpLossRule::NotBlocked};
};

/// Splits the listed units of a sale into runs by F10. In FIFO order, the loss units of the
/// sale take the blocked units first; profitable units are never blocked. Neighbouring runs with
/// the same F10 are one run.
std::vector<SalePart> salePartsOf(const FifoIsinResult& aPool,
                                  const StatementEventReference& aSale,
                                  Units aBlockedUnits) {
    std::vector<SalePart> parts;
    Units blockedLeft = aBlockedUnits;

    const auto append = [&parts](Units aUnits, KdvpLossRule aRule) {
        if (aUnits == 0)
        {
            return;
        }

        if (!parts.empty() && parts.back().mLossRule == aRule)
        {
            parts.back().mUnits += aUnits;
        }
        else
        {
            parts.push_back({.mUnits = aUnits, .mLossRule = aRule});
        }
    };

    for (const auto& match : aPool.mMatches)
    {
        if (match.mSale != aSale || isPastHoldingPeriod(match))
        {
            continue;
        }

        const Units blocked =
            match.mGainTicks.numerator() < 0 ? std::min(blockedLeft, match.mUnits) : Units{0};

        blockedLeft -= blocked;
        append(blocked, KdvpLossRule::Blocked);
        append(match.mUnits - blocked, KdvpLossRule::NotBlocked);
    }

    return parts;
}

/// Builds the list of an ISIN that findBlock did not block. Units past the holding period get no
/// row.
KdvpInventoryList buildList(const std::vector<TradeInstrument>& aInstruments,
                            std::size_t aInstrumentIndex,
                            const ReportingYearLedger& aLedger,
                            const FifoIsinResult& aPool,
                            const LossEligibilityIsin& aLoss) {
    // A purchase used only by earlier sales has no group and gets no row.
    const auto groups = purchaseGroupsOf(aPool);

    KdvpInventoryList list{.mIsin = aPool.mIsin, .mName = aPool.mName};
    Units stock = 0;

    for (std::size_t position = 0; position < aLedger.mCapitalEvents.size(); ++position)
    {
        const auto& event = aLedger.mCapitalEvents.at(position);

        if (event.mKind != StatementEventKind::Trade || event.mInstrumentIndex != aInstrumentIndex)
        {
            continue;
        }

        const auto& trade = aInstruments.at(aInstrumentIndex).mTransactions.at(event.mEventIndex);

        if (trade.mTradeSide == TradeSide::Buy)
        {
            for (const auto& group : groups)
            {
                if (group.mPurchaseIndex != event.mEventIndex)
                {
                    continue;
                }

                // Every sale uses only earlier purchases, so the stock never goes below zero.
                // findBlock checked that the unit value fits F4.
                stock += group.mUnits;
                list.mRows.push_back(
                    {.mEntry = KdvpPurchaseRow{.mPurchase = event,
                                               .mDate = trade.mMetadata.mTaxDate,
                                               .mMethod = KdvpPurchaseMethod::B,
                                               .mUnits = group.mUnits,
                                               .mUnitPrice = std::get<UnitPrice>(
                                                   filingUnitPriceOf(group.mUnitValue))},
                     .mStock = stock});
            }
        }
        else if (position >= aLedger.mHistoricalCapitalEventCount)
        {
            const auto* loss = lossOf(aLoss, event);
            const Units blockedUnits = loss != nullptr ? loss->ineligibleUnits() : Units{0};

            // A sale with only units past the holding period has no part and no row.
            for (const auto& part : salePartsOf(aPool, event, blockedUnits))
            {
                stock -= part.mUnits;
                list.mRows.push_back({.mEntry = KdvpSaleRow{.mSale = event,
                                                            .mDate = trade.mMetadata.mTaxDate,
                                                            .mUnits = part.mUnits,
                                                            .mUnitPrice = trade.mUnitPrice,
                                                            .mLossRule = part.mLossRule},
                                      .mStock = stock});
            }
        }
    }

    return list;
}

} // namespace

CapitalGainsBuildResult CapitalGainsReportBuilder::build(const MergedStatement& aStatement,
                                                         const ReportingYearLedger& aLedger,
                                                         const FifoMatchingResult& aFifo,
                                                         const LossEligibilityResult& aLoss,
                                                         const std::vector<Isin>& aExcludedIsins) {
    CapitalGainsBuildResult result;
    CapitalGainsReportKDVP report;
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    const auto yearSales = collectSelectedYearSales(instruments, aLedger);

    // aFifo.mIsins and aLoss.mIsins follow the instruments, so an index is all three.
    for (std::size_t index = 0; index < instruments.size(); ++index)
    {
        if (yearSales.at(index).empty())
        {
            continue;
        }

        const auto& pool = aFifo.mIsins.at(index);
        const auto& loss = aLoss.mIsins.at(index);
        const bool leftOut = std::find(aExcludedIsins.begin(), aExcludedIsins.end(), pool.mIsin) !=
                             aExcludedIsins.end();
        auto blocked = findBlock(pool, loss, yearSales.at(index));

        if (leftOut)
        {
            result.mExcluded.push_back(KdvpExcludedIsin{
                .mIsin = pool.mIsin,
                .mName = pool.mName,
                .mBlockReason = blocked ? std::optional{blocked->mReason} : std::nullopt,
                .mSales = yearSales.at(index)});

            continue;
        }

        if (blocked)
        {
            result.mBlocked.push_back(std::move(*blocked));

            continue;
        }

        auto list = buildList(instruments, index, aLedger, pool, loss);
        const auto longHeld = collectLongHeldUnits(pool, yearSales.at(index));

        if (!list.mRows.empty())
        {
            report.mLists.push_back(std::move(list));
        }

        result.mLongHeld.insert(result.mLongHeld.end(), longHeld.begin(), longHeld.end());
    }

    if (result.mBlocked.empty())
    {
        result.mReport = std::move(report);
    }

    return result;
}

} // namespace taxbroker
