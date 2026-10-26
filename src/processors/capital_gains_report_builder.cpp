#include "processors/capital_gains_report_builder.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <numeric>
#include <utility>

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

bool isPastHoldingPeriod(const Date& aPurchase, const Date& aSale) {
    constexpr int kFirstYearOfShortPeriod = 2022;
    constexpr int kShortPeriodYears = 15;
    constexpr int kLongPeriodYears = 20;

    const std::chrono::year_month_day purchase{std::chrono::sys_days{aPurchase}};
    const std::chrono::year_month_day sale{std::chrono::sys_days{aSale}};
    const int periodYears = static_cast<int>(sale.year()) >= kFirstYearOfShortPeriod
                                ? kShortPeriodYears
                                : kLongPeriodYears;

    const auto anniversaryYear = purchase.year() + std::chrono::years{periodYears};
    auto anniversary = anniversaryYear / purchase.month() / purchase.day();

    if (!anniversary.ok())
    {
        anniversary = anniversary.year() / anniversary.month() / std::chrono::day{28};
    }

    return std::chrono::sys_days{aSale} > std::chrono::sys_days{anniversary};
}

/// Tells whether the match uses units past the holding period. Such units are tax free.
bool isLongHeld(const FifoMatch& aMatch) {
    return isPastHoldingPeriod(aMatch.mPurchaseDate, aMatch.mSaleDate);
}

/// Why the ISIN gets no list.
std::optional<KdvpBlockedIsin> findBlock(const FifoIsinResult& aPool,
                                         const std::vector<StatementEventReference>& aYearSales) {
    if (aPool.mStatus != FifoIsinStatus::Matched)
    {
        return KdvpBlockedIsin{.mIsin = aPool.mIsin,
                               .mName = aPool.mName,
                               .mReason = KdvpBlockReason::FifoStopped,
                               .mSales = aYearSales};
    }

    std::vector<StatementEventReference> lossSales;

    for (const auto& sale : aYearSales)
    {
        const bool hasLossPortion = std::any_of(aPool.mMatches.begin(),
                                                aPool.mMatches.end(),
                                                [&sale](const FifoMatch& aMatch) {
                                                    return aMatch.mSale == sale &&
                                                           !isLongHeld(aMatch) &&
                                                           aMatch.mGainTicks.numerator() < 0;
                                                });

        if (hasLossPortion)
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
                           .mReason = KdvpBlockReason::LossEligibilityUnresolved,
                           .mSales = std::move(lossSales)};
}

/// Returns the tax-free units of each sale of the year, past the holding period.
std::vector<KdvpLongHeldUnits>
collectLongHeldUnits(const FifoIsinResult& aPool,
                     const std::vector<StatementEventReference>& aYearSales) {
    std::vector<KdvpLongHeldUnits> longHeld;

    for (const auto& sale : aYearSales)
    {
        const Units units = std::accumulate(aPool.mMatches.begin(),
                                            aPool.mMatches.end(),
                                            Units{0},
                                            [&sale](Units aSum, const FifoMatch& aMatch) {
                                                return aMatch.mSale == sale && isLongHeld(aMatch)
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

/// Builds the list of an ISIN that findBlock did not block. Units past the holding period get no
/// row.
KdvpInventoryList buildList(const std::vector<TradeInstrument>& aInstruments,
                            std::size_t aInstrumentIndex,
                            const ReportingYearLedger& aLedger,
                            const FifoIsinResult& aPool) {
    // The units within the holding period that the sales of the year use, by purchase and by
    // sale event index. A purchase used only by earlier sales has no match and gets no row.
    std::map<std::size_t, Units> usedUnits;
    std::map<std::size_t, Units> listedSaleUnits;

    for (const auto& match : aPool.mMatches)
    {
        if (isLongHeld(match))
        {
            continue;
        }

        usedUnits[match.mPurchase.mEventIndex] += match.mUnits;
        listedSaleUnits[match.mSale.mEventIndex] += match.mUnits;
    }

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
            const auto used = usedUnits.find(event.mEventIndex);

            if (used == usedUnits.end())
            {
                continue;
            }

            // Every sale uses only earlier purchases, so the stock never goes below zero.
            stock += used->second;
            list.mRows.push_back({.mEntry = KdvpPurchaseRow{.mPurchase = event,
                                                            .mDate = trade.mMetadata.mTaxDate,
                                                            .mMethod = KdvpPurchaseMethod::B,
                                                            .mUnits = used->second,
                                                            .mUnitPrice = trade.mUnitPrice},
                                  .mStock = stock});
        }
        else if (position >= aLedger.mHistoricalCapitalEventCount)
        {
            const auto listed = listedSaleUnits.find(event.mEventIndex);

            if (listed == listedSaleUnits.end())
            {
                continue;
            }

            stock -= listed->second;
            list.mRows.push_back({.mEntry = KdvpSaleRow{.mSale = event,
                                                        .mDate = trade.mMetadata.mTaxDate,
                                                        .mUnits = listed->second,
                                                        .mUnitPrice = trade.mUnitPrice,
                                                        .mLossRule = KdvpLossRule::NotBlocked},
                                  .mStock = stock});
        }
    }

    return list;
}

} // namespace

CapitalGainsBuildResult CapitalGainsReportBuilder::build(const MergedStatement& aStatement,
                                                         const ReportingYearLedger& aLedger,
                                                         const FifoMatchingResult& aFifo,
                                                         const std::vector<Isin>& aExcludedIsins) {
    CapitalGainsBuildResult result;
    CapitalGainsReportKDVP report;
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    const auto yearSales = collectSelectedYearSales(instruments, aLedger);

    // aFifo.mIsins follows the instruments, so an index is both.
    for (std::size_t index = 0; index < instruments.size(); ++index)
    {
        if (yearSales.at(index).empty())
        {
            continue;
        }

        const auto& pool = aFifo.mIsins.at(index);
        auto blocked = findBlock(pool, yearSales.at(index));

        if (!blocked)
        {
            auto list = buildList(instruments, index, aLedger, pool);
            const auto longHeld = collectLongHeldUnits(pool, yearSales.at(index));

            if (!list.mRows.empty())
            {
                report.mLists.push_back(std::move(list));
            }

            result.mLongHeld.insert(result.mLongHeld.end(), longHeld.begin(), longHeld.end());

            continue;
        }

        const bool leftOut = std::find(aExcludedIsins.begin(), aExcludedIsins.end(), pool.mIsin) !=
                             aExcludedIsins.end();

        (leftOut ? result.mExcluded : result.mBlocked).push_back(std::move(*blocked));
    }

    if (result.mBlocked.empty())
    {
        result.mReport = std::move(report);
    }

    return result;
}

} // namespace taxbroker
