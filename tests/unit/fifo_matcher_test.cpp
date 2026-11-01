#include "processors/fifo_matcher.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

/// A literal number of 10^-16 EUR ticks. One EUR is 10'000'000'000'000'000 ticks.
ExactRational ticks(std::int64_t aTicks) {
    return ExactRational{WideInteger{aTicks}};
}

/// Puts all trades and actions in tax-date order. Equal dates keep the order listed in the test.
void orderChronologically(MergedStatement& aStatement) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    auto& order = aStatement.mChronologicalOrder;

    for (std::size_t instrument = 0; instrument < instruments.size(); ++instrument)
    {
        for (std::size_t trade = 0; trade < instruments[instrument].mTransactions.size(); ++trade)
        {
            order.push_back({StatementEventKind::Trade, instrument, trade});
        }

        for (std::size_t action = 0; action < instruments[instrument].mCorporateActions.size();
             ++action)
        {
            order.push_back({StatementEventKind::CorporateAction, instrument, action});
        }
    }

    const auto taxDate = [&instruments](const StatementEventReference& aReference) {
        const auto& instrument = instruments.at(aReference.mInstrumentIndex.value());

        return aReference.mKind == StatementEventKind::Trade
                   ? instrument.mTransactions.at(aReference.mEventIndex).mMetadata.mTaxDate
                   : instrument.mCorporateActions.at(aReference.mEventIndex).mMetadata.mTaxDate;
    };

    std::stable_sort(order.begin(), order.end(), [&taxDate](const auto& aLeft, const auto& aRight) {
        return taxDate(aLeft) < taxDate(aRight);
    });
}

/// Prepares the real reporting-year ledger and runs the matcher on it.
FifoMatchingResult matchYear(StatementMergeResult& aMerged, int aYear) {
    orderChronologically(aMerged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});

    return FifoMatcher{}.match(aMerged.mStatement, prepared.mLedger.value());
}

/// Prepares the real reporting-year ledger and runs the matcher with confirmed corporate actions.
FifoMatchingResult matchYearWithActions(StatementMergeResult& aMerged,
                                        int aYear,
                                        const std::vector<ConfirmedCorporateAction>& aActions) {
    orderChronologically(aMerged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});

    return FifoMatcher::match(aMerged.mStatement, prepared.mLedger.value(), aActions);
}

/// The source of a Trade Republic split row in history.csv.
SourceReference splitSource(std::size_t aRow) {
    return SourceReference{
        .mBroker = Broker::TradeRepublic,
        .mFilename = SourceFilename::fromPath("history.csv"),
        .mSourceRow = aRow,
        .mTransactionId = "synthetic-split-1",
    };
}

TEST(FifoMatcherTest, SaleUsingTwoPurchasesGivesTwoMatchesWithExactGains) {
    // Example "one sale uses two purchases" in tax_rules.md.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 2'000'000'000,     // 20 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 1'500'000'000,     // 15 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mIsin, "XX0000000001");
    EXPECT_EQ(isin.mName, "Synthetic Share");
    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    EXPECT_FALSE(isin.mStoppedAt.has_value());
    EXPECT_FALSE(isin.mAvailableUnits.has_value());
    ASSERT_EQ(isin.mMatches.size(), 2U);

    const auto& first = isin.mMatches.at(0);

    EXPECT_EQ(first.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(first.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(first.mUnits, 1'000'000'000); // 10 units from the first purchase
    EXPECT_EQ(first.mBasis.mTicks, ticks(1'000'000'000'000'000'000));   // 10 * 10 = EUR 100
    EXPECT_EQ(first.mSaleValueTicks, ticks(3'000'000'000'000'000'000)); // 10 * 30 = EUR 300
    EXPECT_EQ(first.mGainTicks, ticks(2'000'000'000'000'000'000));      // 300 - 100 = EUR 200

    const auto& second = isin.mMatches.at(1);

    EXPECT_EQ(second.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(second.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(second.mUnits, 500'000'000); // 5 units from the second purchase
    EXPECT_EQ(second.mBasis.mTicks, ticks(1'000'000'000'000'000'000));   // 5 * 20 = EUR 100
    EXPECT_EQ(second.mSaleValueTicks, ticks(1'500'000'000'000'000'000)); // 5 * 30 = EUR 150
    EXPECT_EQ(second.mGainTicks, ticks(500'000'000'000'000'000));        // 150 - 100 = EUR 50

    // 15 of the 20 units of the second purchase stay open: 15 * 20 = EUR 300.
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mPurchase,
              (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(isin.mOpenLots.at(0).mPurchaseDate, day("2024-02-10"));
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 1'500'000'000);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks, ticks(3'000'000'000'000'000'000));
}

TEST(FifoMatcherTest, EarlierYearSaleUsesUpUnitsWithoutAMatchInYear2024) {
    // Example "a purchase partly sold in an earlier year" in tax_rules.md.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'500'000'000,     // 15 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-05-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'400'000'000, // EUR 14
                 .mUnits = 600'000'000,       // 6 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 1U);

    const auto& match = isin.mMatches.at(0);

    EXPECT_EQ(match.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(match.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(match.mUnits, 600'000'000);                             // 6 units
    EXPECT_EQ(match.mBasis.mTicks, ticks(600'000'000'000'000'000));   // 6 * 10 = EUR 60
    EXPECT_EQ(match.mSaleValueTicks, ticks(840'000'000'000'000'000)); // 6 * 14 = EUR 84
    EXPECT_EQ(match.mGainTicks, ticks(240'000'000'000'000'000));      // 84 - 60 = EUR 24

    // 15 - 4 - 6 = 5 units stay open with 5 * 10 = EUR 50.
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 500'000'000);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks, ticks(500'000'000'000'000'000));
}

TEST(FifoMatcherTest, SaleInYear2023ReportsOnlyItsOwnMatchAndLeavesElevenUnitsOpen) {
    // Same events as the 2024 test above: the 2024 sale is after the year end and is not used.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'500'000'000,     // 15 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-05-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'400'000'000, // EUR 14
                 .mUnits = 600'000'000,       // 6 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2023);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 1U);

    const auto& match = isin.mMatches.at(0);

    EXPECT_EQ(match.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(match.mUnits, 400'000'000);                             // 4 units
    EXPECT_EQ(match.mBasis.mTicks, ticks(400'000'000'000'000'000));   // 4 * 10 = EUR 40
    EXPECT_EQ(match.mSaleValueTicks, ticks(480'000'000'000'000'000)); // 4 * 12 = EUR 48
    EXPECT_EQ(match.mGainTicks, ticks(80'000'000'000'000'000));       // 48 - 40 = EUR 8

    // 15 - 4 = 11 units stay open with 11 * 10 = EUR 110.
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 1'100'000'000);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks, ticks(1'100'000'000'000'000'000));
}

TEST(FifoMatcherTest, TwoSalesShareOnePurchaseAndTheStockAfterYearEndIsNotSold) {
    // Example "stock left after the year" in tax_rules.md.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-05-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-08-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'100'000'000, // EUR 11
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-10-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'300'000'000, // EUR 13
                 .mUnits = 800'000'000,       // 8 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-15")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'500'000'000, // EUR 15
                 .mUnits = 300'000'000,       // 3 units, after the year end
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 3U);

    // May sale: 4 units of the February purchase.
    EXPECT_EQ(isin.mMatches.at(0).mSale,
              (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(isin.mMatches.at(0).mPurchase,
              (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(isin.mMatches.at(0).mUnits, 400'000'000);
    EXPECT_EQ(isin.mMatches.at(0).mBasis.mTicks, ticks(400'000'000'000'000'000)); // 4 * 10 = 40
    EXPECT_EQ(isin.mMatches.at(0).mGainTicks, ticks(80'000'000'000'000'000));     // 48 - 40 = 8

    // October sale: 6 units of the February purchase.
    EXPECT_EQ(isin.mMatches.at(1).mSale,
              (StatementEventReference{StatementEventKind::Trade, 0, 3}));
    EXPECT_EQ(isin.mMatches.at(1).mPurchase,
              (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(isin.mMatches.at(1).mUnits, 600'000'000);
    EXPECT_EQ(isin.mMatches.at(1).mBasis.mTicks, ticks(600'000'000'000'000'000));   // 6 * 10 = 60
    EXPECT_EQ(isin.mMatches.at(1).mSaleValueTicks, ticks(780'000'000'000'000'000)); // 6 * 13 = 78
    EXPECT_EQ(isin.mMatches.at(1).mGainTicks, ticks(180'000'000'000'000'000));      // 78 - 60 = 18

    // October sale: 2 units of the August purchase.
    EXPECT_EQ(isin.mMatches.at(2).mSale,
              (StatementEventReference{StatementEventKind::Trade, 0, 3}));
    EXPECT_EQ(isin.mMatches.at(2).mPurchase,
              (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(isin.mMatches.at(2).mUnits, 200'000'000);
    EXPECT_EQ(isin.mMatches.at(2).mBasis.mTicks, ticks(220'000'000'000'000'000));   // 2 * 11 = 22
    EXPECT_EQ(isin.mMatches.at(2).mSaleValueTicks, ticks(260'000'000'000'000'000)); // 2 * 13 = 26
    EXPECT_EQ(isin.mMatches.at(2).mGainTicks, ticks(40'000'000'000'000'000));       // 26 - 22 = 4

    // 5 - 2 = 3 units of the August purchase are still held on 31 December: 3 * 11 = EUR 33.
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mPurchase,
              (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 300'000'000);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks, ticks(330'000'000'000'000'000));
}

TEST(FifoMatcherTest, SaleOn31DecemberIsNotMatchedForTheYearButSaleOn1JanuaryIs) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-12-30")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-12-31")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-01-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'400'000'000, // EUR 14
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 1U);

    const auto& match = isin.mMatches.at(0);

    EXPECT_EQ(match.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(match.mSaleDate, day("2024-01-01"));
    EXPECT_EQ(match.mPurchaseDate, day("2023-12-30"));
    EXPECT_EQ(match.mUnits, 100'000'000);
    EXPECT_EQ(match.mBasis.mTicks, ticks(100'000'000'000'000'000));   // 1 * 10 = EUR 10
    EXPECT_EQ(match.mSaleValueTicks, ticks(140'000'000'000'000'000)); // 1 * 14 = EUR 14
    EXPECT_EQ(match.mGainTicks, ticks(40'000'000'000'000'000));       // 14 - 10 = EUR 4

    // 2 - 1 - 1 = 0 units: the fully used lot is gone.
    EXPECT_TRUE(isin.mOpenLots.empty());
}

TEST(FifoMatcherTest, OneFifoPoolSpansBrokersAndFiles) {
    // Cross-broker example in fifo.md: sales at the second broker use purchases at both brokers.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mIsin = "XX0000000001",
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2021-02-10"),
                                       .mSources = {{.mBroker = Broker::TradeRepublic,
                                                     .mFilename =
                                                         SourceFilename::fromPath("tr.csv"),
                                                     .mSourceRow = 2,
                                                     .mInputSequence = {0, 0}}}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 150'000'000,       // 1.5 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2023-04-05"),
                                       .mSources = {{.mBroker = Broker::InteractiveBrokers,
                                                     .mFilename = SourceFilename::fromPath(
                                                         "ibkr.csv"),
                                                     .mSourceRow = 2,
                                                     .mInputSequence = {1, 0}}}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'200'000'000, // EUR 12
                         .mUnits = 40'000'000,        // 0.4 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2023-09-10"),
                                       .mSources = {{.mBroker = Broker::TradeRepublic,
                                                     .mFilename = SourceFilename::fromPath(
                                                         "tr.csv"),
                                                     .mSourceRow = 3,
                                                     .mInputSequence = {0, 1}}}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 75'000'000,        // 0.75 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {{.mBroker = Broker::InteractiveBrokers,
                                                     .mFilename = SourceFilename::fromPath(
                                                         "ibkr.csv"),
                                                     .mSourceRow = 3,
                                                     .mInputSequence = {1, 1}}}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'200'000'000, // EUR 12
                         .mUnits = 160'000'000,       // 1.6 units
                         .mCurrency = Currency::EUR},
                    },
            }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);
    const auto& trades = merged.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions;

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 2U);

    // 1.5 - 0.4 = 1.1 units of the 2021 purchase at the first broker.
    const auto& first = isin.mMatches.at(0);

    EXPECT_EQ(first.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(trades.at(first.mPurchase.mEventIndex).mMetadata.mSources.at(0).mBroker,
              Broker::TradeRepublic);
    EXPECT_EQ(trades.at(first.mSale.mEventIndex).mMetadata.mSources.at(0).mBroker,
              Broker::InteractiveBrokers);
    EXPECT_EQ(first.mPurchaseDate, day("2021-02-10"));
    EXPECT_EQ(first.mSaleDate, day("2024-06-01"));
    EXPECT_EQ(first.mUnits, 110'000'000);
    EXPECT_EQ(first.mBasis.mTicks, ticks(110'000'000'000'000'000));   // 1.1 * 10 = EUR 11
    EXPECT_EQ(first.mSaleValueTicks, ticks(132'000'000'000'000'000)); // 1.1 * 12 = EUR 13.2
    EXPECT_EQ(first.mGainTicks, ticks(22'000'000'000'000'000));       // 13.2 - 11 = EUR 2.2

    // 1.6 - 1.1 = 0.5 units of the 2023 purchase.
    const auto& second = isin.mMatches.at(1);

    EXPECT_EQ(second.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(second.mPurchaseDate, day("2023-09-10"));
    EXPECT_EQ(second.mUnits, 50'000'000);
    EXPECT_EQ(second.mBasis.mTicks, ticks(50'000'000'000'000'000));   // 0.5 * 10 = EUR 5
    EXPECT_EQ(second.mSaleValueTicks, ticks(60'000'000'000'000'000)); // 0.5 * 12 = EUR 6
    EXPECT_EQ(second.mGainTicks, ticks(10'000'000'000'000'000));      // 6 - 5 = EUR 1

    // 0.75 - 0.5 = 0.25 units stay open: 0.25 * 10 = EUR 2.5.
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mPurchase,
              (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 25'000'000);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks, ticks(25'000'000'000'000'000));
}

TEST(FifoMatcherTest, SwappingTheBrokersDoesNotChangeTheMatching) {
    // The same events as the cross-broker test, with the two brokers on the opposite rows.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mIsin = "XX0000000001",
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2021-02-10"),
                                       .mSources = {{.mBroker = Broker::InteractiveBrokers,
                                                     .mFilename =
                                                         SourceFilename::fromPath("ibkr.csv"),
                                                     .mSourceRow = 2,
                                                     .mInputSequence = {0, 0}}}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 150'000'000,       // 1.5 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2023-04-05"),
                                       .mSources = {{.mBroker = Broker::TradeRepublic,
                                                     .mFilename = SourceFilename::fromPath(
                                                         "tr.csv"),
                                                     .mSourceRow = 2,
                                                     .mInputSequence = {1, 0}}}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'200'000'000, // EUR 12
                         .mUnits = 40'000'000,        // 0.4 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2023-09-10"),
                                       .mSources = {{.mBroker = Broker::InteractiveBrokers,
                                                     .mFilename = SourceFilename::fromPath(
                                                         "ibkr.csv"),
                                                     .mSourceRow = 3,
                                                     .mInputSequence = {0, 1}}}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 75'000'000,        // 0.75 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {{.mBroker = Broker::TradeRepublic,
                                                     .mFilename = SourceFilename::fromPath(
                                                         "tr.csv"),
                                                     .mSourceRow = 3,
                                                     .mInputSequence = {1, 1}}}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'200'000'000, // EUR 12
                         .mUnits = 160'000'000,       // 1.6 units
                         .mCurrency = Currency::EUR},
                    },
            }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 2U);

    EXPECT_EQ(isin.mMatches.at(0).mPurchase,
              (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(isin.mMatches.at(0).mUnits, 110'000'000); // 1.5 - 0.4 = 1.1 units
    EXPECT_EQ(isin.mMatches.at(1).mPurchase,
              (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(isin.mMatches.at(1).mUnits, 50'000'000); // 1.6 - 1.1 = 0.5 units

    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 25'000'000); // 0.75 - 0.5 = 0.25 units
}

TEST(FifoMatcherTest, DifferentIsinsUseSeparatePools) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share A",
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Share B",
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 2U);

    const auto& shareA = result.mIsins.at(0);

    EXPECT_EQ(shareA.mIsin, "XX0000000001");
    EXPECT_EQ(shareA.mStatus, FifoIsinStatus::Matched);
    EXPECT_TRUE(shareA.mMatches.empty());
    ASSERT_EQ(shareA.mOpenLots.size(), 1U);
    EXPECT_EQ(shareA.mOpenLots.at(0).mState.mRemainingUnits, 100'000'000); // untouched

    const auto& shareB = result.mIsins.at(1);

    EXPECT_EQ(shareB.mIsin, "XX0000000002");
    EXPECT_EQ(shareB.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(shareB.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 1, 0}));
    EXPECT_EQ(shareB.mAvailableUnits, 0);
    EXPECT_TRUE(shareB.mMatches.empty());
    EXPECT_TRUE(shareB.mOpenLots.empty());

    // Only B has an error; the purchase of A is never used for B.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mIsin, "XX0000000002");
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mTaxDate, day("2024-02-01"));
}

TEST(FifoMatcherTest, FractionalUnitsAreMatchedExactlyAndAFullySoldLotIsRemoved) {
    // Example "Exact fractional units" in fifo.md: 0.75 - 0.20 = 0.55, then 0.55 - 0.55 = 0.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 75'000'000,        // 0.75 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 20'000'000,        // 0.2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 55'000'000,        // 0.55 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 2U);

    EXPECT_EQ(isin.mMatches.at(0).mUnits, 20'000'000);
    EXPECT_EQ(isin.mMatches.at(0).mBasis.mTicks, ticks(20'000'000'000'000'000));   // 0.2 * 10 = 2
    EXPECT_EQ(isin.mMatches.at(0).mSaleValueTicks, ticks(24'000'000'000'000'000)); // 0.2 * 12 = 2.4
    EXPECT_EQ(isin.mMatches.at(0).mGainTicks, ticks(4'000'000'000'000'000));       // 2.4 - 2 = 0.4

    EXPECT_EQ(isin.mMatches.at(1).mUnits, 55'000'000);
    EXPECT_EQ(isin.mMatches.at(1).mBasis.mTicks, ticks(55'000'000'000'000'000)); // 0.55 * 10 = 5.5
    EXPECT_EQ(isin.mMatches.at(1).mSaleValueTicks,
              ticks(66'000'000'000'000'000));                                 // 0.55 * 12 = 6.6
    EXPECT_EQ(isin.mMatches.at(1).mGainTicks, ticks(11'000'000'000'000'000)); // 6.6 - 5.5 = 1.1

    EXPECT_TRUE(isin.mOpenLots.empty());
}

TEST(FifoMatcherTest, ThreePartSalesTakeTheExactBasisOfTheLotAndNothingMore) {
    // One lot of 1 unit at EUR 10.00000001 has a basis of 10.00000001 * 1 = EUR 10.00000001.
    // Three sales of 0.33333333, 0.33333333 and 0.33333334 units take 3.3333333033333333,
    // 3.3333333033333333 and 3.3333334033333334 EUR, which add back to the whole basis.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'001, // EUR 10.00000001
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 33'333'333,        // 0.33333333 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 33'333'333,        // 0.33333333 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 33'333'334,        // 0.33333334 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 3U);

    EXPECT_EQ(isin.mMatches.at(0).mBasis.mTicks, ticks(33'333'333'033'333'333));
    EXPECT_EQ(isin.mMatches.at(0).mSaleValueTicks, ticks(33'333'333'000'000'000));
    EXPECT_EQ(isin.mMatches.at(0).mGainTicks, ticks(-33'333'333)); // a loss of 0.0000000033333333

    EXPECT_EQ(isin.mMatches.at(1).mBasis.mTicks, ticks(33'333'333'033'333'333));
    EXPECT_EQ(isin.mMatches.at(1).mSaleValueTicks, ticks(33'333'333'000'000'000));
    EXPECT_EQ(isin.mMatches.at(1).mGainTicks, ticks(-33'333'333));

    EXPECT_EQ(isin.mMatches.at(2).mBasis.mTicks, ticks(33'333'334'033'333'334));
    EXPECT_EQ(isin.mMatches.at(2).mSaleValueTicks, ticks(33'333'334'000'000'000));
    EXPECT_EQ(isin.mMatches.at(2).mGainTicks, ticks(-33'333'334));

    // 33'333'333'033'333'333 + 33'333'333'033'333'333 + 33'333'334'033'333'334
    //     = 100'000'000'100'000'000 ticks = EUR 10.00000001, the whole lot basis.
    EXPECT_TRUE(isin.mOpenLots.empty());
}

TEST(FifoMatcherTest, ProfitableSaleKeepsItsLossMakingPortionAsANegativeGain) {
    // The first 10 units cost EUR 10 and give +100; the next 5 cost EUR 30 and give -50.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 1'500'000'000,     // 15 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    ASSERT_EQ(isin.mMatches.size(), 2U);

    EXPECT_EQ(isin.mMatches.at(0).mUnits, 1'000'000'000);
    EXPECT_EQ(isin.mMatches.at(0).mBasis.mTicks, ticks(1'000'000'000'000'000'000));   // 10 * 10
    EXPECT_EQ(isin.mMatches.at(0).mSaleValueTicks, ticks(2'000'000'000'000'000'000)); // 10 * 20
    EXPECT_EQ(isin.mMatches.at(0).mGainTicks, ticks(1'000'000'000'000'000'000));      // +EUR 100

    EXPECT_EQ(isin.mMatches.at(1).mUnits, 500'000'000);
    EXPECT_EQ(isin.mMatches.at(1).mBasis.mTicks, ticks(1'500'000'000'000'000'000));   // 5 * 30
    EXPECT_EQ(isin.mMatches.at(1).mSaleValueTicks, ticks(1'000'000'000'000'000'000)); // 5 * 20
    EXPECT_EQ(isin.mMatches.at(1).mGainTicks, ticks(-500'000'000'000'000'000));       // -EUR 50
}

TEST(FifoMatcherTest, EarlierSalesKeepTheirMatchesApart) {
    // The 2023 sale only uses up lots for the 2024 report, but its match is kept apart for the
    // 30-day loss rule: an earlier loss takes its replacement units first.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-05-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    ASSERT_EQ(isin.mEarlierMatches.size(), 1U);

    const auto& earlier = isin.mEarlierMatches.at(0);

    EXPECT_EQ(earlier.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(earlier.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(earlier.mSaleDate, day("2023-05-10"));
    EXPECT_EQ(earlier.mUnits, 400'000'000);
    EXPECT_EQ(earlier.mGainTicks, ticks(-80'000'000'000'000'000)); // 4 * (8 - 10) = -EUR 8

    ASSERT_EQ(isin.mMatches.size(), 1U);

    const auto& selected = isin.mMatches.at(0);

    EXPECT_EQ(selected.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(selected.mUnits, 200'000'000);
    EXPECT_EQ(selected.mGainTicks, ticks(40'000'000'000'000'000)); // 2 * (12 - 10) = +EUR 4
}

TEST(FifoMatcherTest, SaleAtThePurchasePriceHasAGainOfExactlyZero) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    ASSERT_EQ(result.mIsins.at(0).mMatches.size(), 1U);

    const auto& match = result.mIsins.at(0).mMatches.at(0);

    EXPECT_EQ(match.mBasis.mTicks, ticks(200'000'000'000'000'000));   // 2 * 10 = EUR 20
    EXPECT_EQ(match.mSaleValueTicks, ticks(200'000'000'000'000'000)); // 2 * 10 = EUR 20
    EXPECT_EQ(match.mGainTicks, ticks(0));
}

TEST(FifoMatcherTest, ShortSaleStopsTheIsinAndLeavesTheLotsAsTheyWereBeforeIt) {
    // Example "Incomplete-history example" in fifo.md.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2022-03-01")},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 60'000'000,        // 0.6 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2023-08-01")},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'200'000'000, // EUR 12
                         .mUnits = 25'000'000,        // 0.25 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-05-01"),
                                       .mSources = {{.mBroker = Broker::TradeRepublic,
                                                     .mFilename =
                                                         SourceFilename::fromPath("tr.csv"),
                                                     .mSourceRow = 4,
                                                     .mTransactionId = "synthetic-sale-may",
                                                     .mInputSequence = {0, 2}}}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'200'000'000, // EUR 12
                         .mUnits = 50'000'000,        // 0.5 units, but only 0.35 are open
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-06-01")},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit, ignored after the stop
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-07-01")},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'200'000'000, // EUR 12
                         .mUnits = 10'000'000,        // 0.1 units, ignored after the stop
                         .mCurrency = Currency::EUR},
                    },
            }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(isin.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(isin.mAvailableUnits, 35'000'000); // 0.6 - 0.25 = 0.35 units
    EXPECT_TRUE(isin.mMatches.empty());

    // The failing sale is not applied partly: the lot keeps 0.35 units with 0.35 * 10 = EUR 3.5.
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mPurchase,
              (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(isin.mOpenLots.at(0).mPurchaseDate, day("2022-03-01"));
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 35'000'000);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks, ticks(35'000'000'000'000'000));

    // One error for the May sale. The June purchase and the July sale give no error of their own.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::IncompleteHistory);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_EQ(diagnostic.mMessage,
              "Earlier purchase history is missing: this sale needs more units than the earlier "
              "purchases left. Add the older or missing statements.");
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::HistoricalValidation);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Synthetic Share");
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-05-01"));

    ASSERT_EQ(diagnostic.mContext.mSources.size(), 1U);

    EXPECT_EQ(diagnostic.mContext.mSources.at(0).mFilename.value(), "tr.csv");
    EXPECT_EQ(diagnostic.mContext.mSources.at(0).mSourceRow, 4U);
    EXPECT_EQ(diagnostic.mContext.mSources.at(0).mTransactionId, "synthetic-sale-may");
}

TEST(FifoMatcherTest, ShortSaleKeepsTheMatchesOfEarlierSalesInTheYear) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 100'000'000,       // 1 unit, matched
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 300'000'000,       // 3 units, but only 2 are open
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(isin.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(isin.mAvailableUnits, 200'000'000); // 3 - 1 = 2 units

    ASSERT_EQ(isin.mMatches.size(), 1U);
    EXPECT_EQ(isin.mMatches.at(0).mSale,
              (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(isin.mMatches.at(0).mUnits, 100'000'000);
    EXPECT_EQ(isin.mMatches.at(0).mGainTicks, ticks(20'000'000'000'000'000)); // 12 - 10 = EUR 2

    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 200'000'000);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks, ticks(200'000'000'000'000'000));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mTaxDate, day("2024-03-01"));
}

TEST(FifoMatcherTest, LaterPurchaseOnTheSameDayCannotRepairAnEarlierSale) {
    // The sale comes first in the ledger order, so the purchase after it is not available to it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-05-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-05-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(isin.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(isin.mAvailableUnits, 0);
    EXPECT_TRUE(isin.mMatches.empty());
    EXPECT_TRUE(isin.mOpenLots.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mTaxDate, day("2024-05-01"));
}

TEST(FifoMatcherTest, SaleOneSmallestStepAboveTheOpenUnitsIsReportedAsIncompleteHistory) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 35'000'000,        // 0.35 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 35'000'001,        // 0.35000001 units, one step more than open
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(isin.mAvailableUnits, 35'000'000);
    EXPECT_TRUE(isin.mMatches.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::IncompleteHistory);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mTaxDate, day("2024-02-01"));
}

TEST(FifoMatcherTest, SaleOfExactlyTheOpenUnitsIsNotIncompleteHistory) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 35'000'000,        // 0.35 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 35'000'000,        // 0.35 units, exactly the open units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 1U);
    EXPECT_EQ(isin.mMatches.at(0).mUnits, 35'000'000);
    EXPECT_TRUE(isin.mOpenLots.empty());
}

TEST(FifoMatcherTest, ShortSaleInAnEarlierYearBlocksTheIsinForTheSelectedYear) {
    // fifo.md, "Trades before the selected year": an unmatched older sale is still an error.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-04-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 200'000'000,       // 2 units, but only 1 is open
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units, ignored after the stop
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 100'000'000,       // 1 unit, ignored after the stop
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(isin.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(isin.mAvailableUnits, 100'000'000);
    EXPECT_TRUE(isin.mMatches.empty()); // the 2024 sale was not matched

    // The error is for the 2023 sale, not for anything in the selected year.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mTaxDate, day("2023-04-01"));
}

TEST(FifoMatcherTest, ShortSaleInAnEarlierYearIsReportedWithoutAnyEventInTheSelectedYear) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-04-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 200'000'000,       // 2 units, but only 1 is open
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_EQ(result.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mTaxDate, day("2023-04-01"));
}

TEST(FifoMatcherTest, OnlyTheFirstShortSaleOfAnIsinIsReported) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 200'000'000,       // 2 units, but only 1 is open: the error
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 300'000'000,       // 3 units, ignored after the stop
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(isin.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_TRUE(isin.mMatches.empty());
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 100'000'000);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mTaxDate, day("2024-02-01"));
}

TEST(FifoMatcherTest, ShortSalesOfSeveralIsinsGiveOneErrorEachAndAnotherIsinStillMatches) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share A",
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 200'000'000,       // 2 units, but only 1 is open
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Share B",
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-15")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 100'000'000,       // 1 unit, complete
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Share C",
            .mIsin = "XX0000000003",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 200'000'000,       // 2 units, but only 1 is open
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 3U);
    EXPECT_EQ(result.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(result.mIsins.at(1).mStatus, FifoIsinStatus::Matched);
    EXPECT_EQ(result.mIsins.at(1).mMatches.size(), 1U);
    EXPECT_EQ(result.mIsins.at(2).mStatus, FifoIsinStatus::InsufficientUnits);

    // Ledger order: C (2024-02-01) before A (2024-03-01). B has no error.
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mIsin, "XX0000000003");
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mInstrumentName, "Synthetic Share C");
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mTaxDate, day("2024-02-01"));
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mIsin, "XX0000000001");
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mInstrumentName, "Synthetic Share A");
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mTaxDate, day("2024-03-01"));
}

TEST(FifoMatcherTest, ShortSaleMergedFromTwoFilesKeepsBothSourcesInTheError) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-02-01"),
                                       .mSources = {{.mBroker = Broker::TradeRepublic,
                                                     .mFilename = SourceFilename::fromPath("a.csv"),
                                                     .mSourceRow = 3,
                                                     .mTransactionId = "synthetic-sale",
                                                     .mInputSequence = {0, 1}},
                                                    {.mBroker = Broker::TradeRepublic,
                                                     .mFilename = SourceFilename::fromPath("b.csv"),
                                                     .mSourceRow = 2,
                                                     .mTransactionId = "synthetic-sale",
                                                     .mInputSequence = {1, 0}}}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'200'000'000, // EUR 12
                         .mUnits = 100'000'000,       // 1 unit, but nothing was bought
                         .mCurrency = Currency::EUR},
                    },
            }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_EQ(result.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& sources = result.mDiagnostics.at(0).mContext.mSources;

    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(0).mFilename.value(), "a.csv");
    EXPECT_EQ(sources.at(0).mSourceRow, 3U);
    EXPECT_EQ(sources.at(1).mFilename.value(), "b.csv");
    EXPECT_EQ(sources.at(1).mSourceRow, 2U);
}

TEST(FifoMatcherTest, SaleAfterASplitIsNotReportedAsIncompleteHistory) {
    // After a 2-for-1 split, 1 bought unit becomes 2, so selling 2 is not a shortage. The pool
    // stops at the split, so the sale is never compared with the 1 unit open before the split.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 200'000'000,     // 2 units, the position after the split
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-01")},
                 .mType = CorporateActionType::Split,
                 .mRatio = SplitRatio{.mNewShares = 2, .mOldShares = 1}},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::CorporateAction);
    EXPECT_EQ(isin.mStoppedAt,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 0}));
    EXPECT_TRUE(isin.mMatches.empty());
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 100'000'000); // not adjusted
}

TEST(FifoMatcherTest, OpenUnitsThatOverflowInt64StopTheIsinWithAnOverflowDiagnostic) {
    // 50'000'000'000 units are 5'000'000'000'000'000'000 raw units. Two lots add up to
    // 10'000'000'000'000'000'000, which is above the largest int64 (9'223'372'036'854'775'807).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-01-02")},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 100'000'000,           // EUR 1
                         .mUnits = 5'000'000'000'000'000'000, // 50'000'000'000 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-01-03")},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 100'000'000,           // EUR 1
                         .mUnits = 5'000'000'000'000'000'000, // 50'000'000'000 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-02-01"),
                                       .mSources = {{.mBroker = Broker::TradeRepublic,
                                                     .mFilename =
                                                         SourceFilename::fromPath("tr.csv"),
                                                     .mSourceRow = 4,
                                                     .mInputSequence = {0, 2}}}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 100'000'000, // EUR 1
                         .mUnits = 100'000'000,     // 1 unit
                         .mCurrency = Currency::EUR},
                    },
            }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::NumericFailure);
    EXPECT_EQ(isin.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_TRUE(isin.mMatches.empty());
    EXPECT_EQ(isin.mOpenLots.size(), 2U); // both purchases are unchanged

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::ArithmeticOverflow);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::ArithmeticOverflow);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Synthetic Share");
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-02-01"));
    ASSERT_EQ(diagnostic.mContext.mSources.size(), 1U);
    EXPECT_EQ(diagnostic.mContext.mSources.at(0).mFilename.value(), "tr.csv");
    EXPECT_EQ(diagnostic.mContext.mSources.at(0).mSourceRow, 4U);
}

TEST(FifoMatcherTest, PurchaseWithUnitPriceZeroStopsTheIsinWithAnUnrepresentableValueDiagnostic) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-01-02"),
                                       .mSources = {{.mBroker = Broker::TradeRepublic,
                                                     .mFilename =
                                                         SourceFilename::fromPath("tr.csv"),
                                                     .mSourceRow = 2,
                                                     .mInputSequence = {0, 0}}}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 0, // a price the importer would never produce
                         .mUnits = 100'000'000,
                         .mCurrency = Currency::EUR},
                    },
            }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::NumericFailure);
    EXPECT_EQ(isin.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_TRUE(isin.mOpenLots.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::UnrepresentableValue);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::UnrepresentableValue);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-01-02"));
    ASSERT_EQ(diagnostic.mContext.mSources.size(), 1U);
    EXPECT_EQ(diagnostic.mContext.mSources.at(0).mSourceRow, 2U);
}

TEST(FifoMatcherTest, SaleWithZeroUnitsStopsTheIsinAndLeavesTheLotAlone) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 0,                 // a quantity the importer would never produce
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::NumericFailure);
    EXPECT_EQ(isin.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_TRUE(isin.mMatches.empty());
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 100'000'000);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::UnrepresentableValue);
}

TEST(FifoMatcherTest, SaleWithUnitPriceZeroStopsTheIsinAndLeavesTheLotAlone) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 0, // a price the importer would never produce
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::NumericFailure);
    EXPECT_EQ(isin.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_TRUE(isin.mMatches.empty());
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 100'000'000);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::UnrepresentableValue);
}

TEST(FifoMatcherTest, CorporateActionStopsOnlyItsOwnIsin) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 100'000'000,       // 1 unit, ignored after the stop
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-06-01")},
                     .mType = CorporateActionType::Split,
                     .mRatio = SplitRatio{.mNewShares = 2, .mOldShares = 1}},
                },
        },
        {
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 2U);
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& split = result.mIsins.at(0);

    EXPECT_EQ(split.mStatus, FifoIsinStatus::CorporateAction);
    EXPECT_EQ(split.mStoppedAt,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 0}));
    EXPECT_TRUE(split.mMatches.empty());
    ASSERT_EQ(split.mOpenLots.size(), 1U);
    EXPECT_EQ(split.mOpenLots.at(0).mState.mRemainingUnits, 200'000'000); // not adjusted
    EXPECT_EQ(split.mOpenLots.at(0).mState.mRemainingBasis.mTicks, ticks(200'000'000'000'000'000));

    EXPECT_EQ(result.mIsins.at(1).mStatus, FifoIsinStatus::Matched);
    EXPECT_EQ(result.mIsins.at(1).mOpenLots.size(), 1U);
}

TEST(FifoMatcherTest, APurchaseOnTheDayOfASplitIsNotApplied) {
    // fifo.md: an action comes before the trades of its day. The purchase of 1 unit on the split
    // day is earlier in the ledger, but the pool stops before it; the day before still counts.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-05-31")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit, on the split day
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& isin = result.mIsins.at(0);

    // The purchase is listed first on that day, but the pool stops at the split.
    EXPECT_EQ(isin.mStatus, FifoIsinStatus::CorporateAction);
    EXPECT_EQ(isin.mStoppedAt,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 0}));
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mPurchase,
              (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 300'000'000); // 3 units
}

TEST(FifoMatcherTest, ASaleOnTheDayOfASplitIsNotMatched) {
    // The sale of 1 unit on the split day comes after the split, so it is not matched before it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 100'000'000,       // 1 unit, on the split day
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::CorporateAction);
    EXPECT_EQ(isin.mStoppedAt,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 0}));
    EXPECT_TRUE(isin.mMatches.empty());
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 300'000'000); // 3 units, none sold
}

TEST(FifoMatcherTest, ForeignPurchaseStopsTheIsinWithoutCreatingALot) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // USD 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::USD},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 100'000'000,       // 1 unit, ignored after the stop
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::NonEurTrade);
    EXPECT_EQ(isin.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_TRUE(isin.mMatches.empty());
    EXPECT_TRUE(isin.mOpenLots.empty());
}

TEST(FifoMatcherTest, ForeignSaleStopsTheIsinAndLeavesTheEurLotAlone) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // GBP 12
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::GBP},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::NonEurTrade);
    EXPECT_EQ(isin.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_TRUE(isin.mMatches.empty());
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 100'000'000);
}

TEST(FifoMatcherTest, TradeWithUnknownCurrencyIsNotTreatedAsEur) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // price in an unknown currency
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::Unknown},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.mIsins.at(0).mStatus, FifoIsinStatus::NonEurTrade);
    EXPECT_TRUE(result.mIsins.at(0).mOpenLots.empty());
}

TEST(FifoMatcherTest, TradesAfterTheYearEndAreNotInTheLedgerAndAreNotMatched) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 100'000'000,       // 1 unit, no purchase before it
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = matchYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_EQ(result.mIsins.at(0).mStatus, FifoIsinStatus::Matched);
    EXPECT_FALSE(result.mIsins.at(0).mStoppedAt.has_value());
    EXPECT_TRUE(result.mIsins.at(0).mMatches.empty());
    EXPECT_TRUE(result.mIsins.at(0).mOpenLots.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(FifoMatcherTest, NoTradeInstrumentsGiveAnEmptyResult) {
    StatementMergeResult merged;

    const auto result = matchYear(merged, 2024);

    EXPECT_TRUE(result.mIsins.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(FifoMatcherTest, SplitThenReverseSplitKeepsThePurchaseDateAndBasis) {
    // fifo.md, "Split before and reverse split during the selected year": 3 units at EUR 120
    // (EUR 360), 2 / 1 makes 6 units at EUR 60, 1 / 3 makes 2 units at EUR 180, and the June
    // sale of 1 unit takes it from the 2022 lot.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-05-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 12'000'000'000, // EUR 120
                 .mUnits = 300'000'000,        // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 20'000'000'000, // EUR 200
                 .mUnits = 100'000'000,        // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-08-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-04-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2023-08-01")},
                              {.mEvents = {{StatementEventKind::CorporateAction, 0, 1}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 3},
                               .mEffectiveDate = day("2024-04-01")}});

    // Both ratios divide the units exactly, so nothing is rounded.
    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    EXPECT_EQ(isin.mAppliedActions,
              (std::vector<AppliedCorporateAction>{
                  {.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                   .mEffectiveDate = day("2023-08-01"),
                   .mRatio = {.mNewShares = 2, .mOldShares = 1}},
                  {.mEvents = {{StatementEventKind::CorporateAction, 0, 1}},
                   .mEffectiveDate = day("2024-04-01"),
                   .mRatio = {.mNewShares = 1, .mOldShares = 3}}}));
    ASSERT_EQ(isin.mMatches.size(), 1U);

    const auto& match = isin.mMatches.at(0);

    EXPECT_EQ(match.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(match.mPurchaseDate, day("2022-05-10"));
    EXPECT_EQ(match.mUnits, 100'000'000);                               // 1 adjusted unit
    EXPECT_EQ(match.mBasis.mTicks, ticks(1'800'000'000'000'000'000));   // 360 / 2 = EUR 180
    EXPECT_EQ(match.mSaleValueTicks, ticks(2'000'000'000'000'000'000)); // EUR 200
    EXPECT_EQ(match.mGainTicks, ticks(200'000'000'000'000'000));        // 200 - 180 = EUR 20

    ASSERT_EQ(isin.mOpenLots.size(), 1U);

    const auto& lot = isin.mOpenLots.at(0);

    EXPECT_EQ(lot.mPurchaseDate, day("2022-05-10"));
    EXPECT_EQ(lot.mState.mRemainingUnits, 100'000'000); // 2 - 1 = 1 unit
    EXPECT_EQ(lot.mState.mRemainingBasis.mTicks, ticks(1'800'000'000'000'000'000)); // EUR 180
}

TEST(FifoMatcherTest, LargestRemainderGivesTheExtraUnitToTheLargerRemainder) {
    // fifo.md, "Exact distribution across several lots": lots of 1 and 2 units, 5 / 3. Exactly
    // 1.666666666... and 3.333333333...; kept to 8 decimals 1.66666666 + 3.33333333 = 4.99999999,
    // so the missing 0.00000001 goes to the first lot (remainder 2/3 against 1/3). The total is
    // exactly 5, so nothing is rounded away.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 5, .mOldShares = 3},
                               .mEffectiveDate = day("2024-03-01")}});

    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& lots = result.mIsins.at(0).mOpenLots;

    ASSERT_EQ(lots.size(), 2U);
    EXPECT_EQ(lots.at(0).mState.mRemainingUnits, 166'666'667); // 1.66666667
    EXPECT_EQ(lots.at(1).mState.mRemainingUnits, 333'333'333); // 3.33333333
    // The total purchase values stay EUR 10 and EUR 20.
    EXPECT_EQ(lots.at(0).mState.mRemainingBasis.mTicks, ticks(100'000'000'000'000'000));
    EXPECT_EQ(lots.at(1).mState.mRemainingBasis.mTicks, ticks(200'000'000'000'000'000));
    // The values per unit follow from them: 10 / 1.66666667 = 1'000'000'000 / 166'666'667, about
    // 5.99999999, and 20 / 3.33333333 = 2'000'000'000 / 333'333'333, about 6.00000001.
    EXPECT_EQ(exactLotUnitValue(lots.at(0).mState),
              ExactRational::create(1'000'000'000, 166'666'667));
    EXPECT_EQ(exactLotUnitValue(lots.at(1).mState),
              ExactRational::create(2'000'000'000, 333'333'333));
}

TEST(FifoMatcherTest, ASubStepResidualIsRoundedWithAWarning) {
    // fifo.md, "A unit-only action with a sub-step residual": 1 unit with EUR 10, 1 / 3. Exactly
    // 1/3, rounded once to 0.33333333; the rounded-away 1/300000000 unit is worth
    // 1/300000000 x (10 / 0.33333333) = EUR 0.0000001, far below EUR 0.01.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"), .mSources = {splitSource(3)}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 3},
                               .mEffectiveDate = day("2024-03-01")}});

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 33'333'333); // 0.33333333
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks,
              ticks(100'000'000'000'000'000)); // still EUR 10

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionQuantityRounded);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionQuantityRounded);
    EXPECT_TRUE(diagnostic.mMessageParams.empty()); // no units or amounts in the message
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::CorporateAction);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-03-01"));
    EXPECT_EQ(diagnostic.mContext.mSources, (std::vector<SourceReference>{splitSource(3)}));
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Synthetic Share");
}

TEST(FifoMatcherTest, ARoundingWorthOneCentOrMoreStopsThePool) {
    // 1 unit at EUR 9,000,000, 1 / 3: the rounded-away 1/300000000 unit is worth
    // 1/300000000 x (9,000,000 / 0.33333333) = EUR 0.0900000009, not below EUR 0.01.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 900'000'000'000'000, // EUR 9,000,000
                 .mUnits = 100'000'000,             // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"), .mSources = {splitSource(3)}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 3},
                               .mEffectiveDate = day("2024-03-01")}});

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::NumericFailure);
    EXPECT_EQ(isin.mStoppedAt,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 0}));
    EXPECT_TRUE(isin.mAppliedActions.empty());
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 100'000'000); // unchanged

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::CorporateActionUnrepresentable);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionUnrepresentableRoundingBudget);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mSources,
              (std::vector<SourceReference>{splitSource(3)}));
}

TEST(FifoMatcherTest, TheRoundingOfTwoActionsCountsTogether) {
    // 1 unit at EUR 600,000. March 1 / 3: 0.33333333 units, rounding worth
    // 1/300000000 x (600,000 / 0.33333333) = EUR 0.0060000001, a warning. June 3 / 2:
    // 0.33333333 x 3 / 2 = 0.499999995, rounded to 0.50000000 (half away from zero); the
    // 0.000000005 unit is worth 0.000000005 x (600,000 / 0.5) = EUR 0.006. Each is below EUR 0.01,
    // together EUR 0.0120000001 is not, so June stops the pool.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 60'000'000'000'000, // EUR 600,000
                 .mUnits = 100'000'000,            // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 3},
                               .mEffectiveDate = day("2024-03-01")},
                              {.mEvents = {{StatementEventKind::CorporateAction, 0, 1}},
                               .mRatio = {.mNewShares = 3, .mOldShares = 2},
                               .mEffectiveDate = day("2024-06-01")}});

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::NumericFailure);
    EXPECT_EQ(isin.mStoppedAt,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 1}));
    ASSERT_EQ(isin.mAppliedActions.size(), 1U); // only March
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 33'333'333); // after March only

    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionQuantityRounded);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mTaxDate, day("2024-03-01"));
    EXPECT_EQ(result.mDiagnostics.at(1).mMessageKey,
              MessageKey::CorporateActionUnrepresentableRoundingBudget);
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mTaxDate, day("2024-06-01"));
}

// The next four tests use a 1 / 3 reverse split of one unit. It gives 0.33333333 units; the
// rounded-away 1/300000000 unit is worth 1/300000000 x (P / 0.33333333) = P / 99,999,999 for a
// purchase price P. So EUR 999,999.99 is worth exactly EUR 0.01 and EUR 499,999.995 exactly
// EUR 0.005. The limit is EUR 0.01 itself: only a total strictly below it is accepted.

TEST(FifoMatcherTest, ARoundingWorthExactlyOneCentStopsThePool) {
    // P = EUR 999,999.99: 999,999.99 / 99,999,999 = 0.01 exactly.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 99'999'999'000'000, // EUR 999,999.99
                 .mUnits = 100'000'000,            // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"), .mSources = {splitSource(3)}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 3},
                               .mEffectiveDate = day("2024-03-01")}});

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::NumericFailure);
    EXPECT_EQ(isin.mStoppedAt,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 0}));
    EXPECT_TRUE(isin.mAppliedActions.empty());
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 100'000'000); // unchanged, 1 unit

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionUnrepresentableRoundingBudget);
}

TEST(FifoMatcherTest, ARoundingWorthJustBelowOneCentIsAppliedWithAWarning) {
    // P = EUR 999,999.98: 999,999.98 / 99,999,999 = 0.0099999999..., below EUR 0.01.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 99'999'998'000'000, // EUR 999,999.98
                 .mUnits = 100'000'000,            // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"), .mSources = {splitSource(3)}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 3},
                               .mEffectiveDate = day("2024-03-01")}});

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    EXPECT_EQ(isin.mAppliedActions.size(), 1U);
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 33'333'333); // 0.33333333
    // Still EUR 999,999.98: 99,999,998 cents x 10^14 ticks a cent, more than int64 holds.
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks,
              ExactRational{WideInteger{99'999'998} * 100'000'000'000'000});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionQuantityRounded);
}

TEST(FifoMatcherTest, TwoRoundingsAddingUpToExactlyOneCentStopTheSecondAction) {
    // Both purchases are at P = EUR 499,999.995, worth 0.005 exactly each. February: 0.005, a
    // warning. The sale empties the pool. May: the budget is 0.005 + 0.005 = 0.01, not below it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 49'999'999'500'000, // EUR 499,999.995
                 .mUnits = 100'000'000,            // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 50'000'000'000'000, // EUR 500,000
                 .mUnits = 33'333'333,             // 0.33333333 units, all of the lot
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 49'999'999'500'000, // EUR 499,999.995
                 .mUnits = 100'000'000,            // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-05-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 3},
                               .mEffectiveDate = day("2024-02-01")},
                              {.mEvents = {{StatementEventKind::CorporateAction, 0, 1}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 3},
                               .mEffectiveDate = day("2024-05-01")}});

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::NumericFailure);
    EXPECT_EQ(isin.mStoppedAt,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 1}));
    EXPECT_EQ(isin.mAppliedActions.size(), 1U); // only February
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits,
              100'000'000); // the April unit, unchanged

    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionQuantityRounded);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mTaxDate, day("2024-02-01"));
    EXPECT_EQ(result.mDiagnostics.at(1).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.at(1).mMessageKey,
              MessageKey::CorporateActionUnrepresentableRoundingBudget);
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mTaxDate, day("2024-05-01"));
}

TEST(FifoMatcherTest, TwoRoundingsAddingUpToJustBelowOneCentAreBothApplied) {
    // February: P = EUR 499,999.995, worth 0.005. May: P = EUR 499,999.99, worth
    // 499,999.99 / 99,999,999 = 0.00499999995..., so the total is 0.00999999995..., below 0.01.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 49'999'999'500'000, // EUR 499,999.995
                 .mUnits = 100'000'000,            // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 50'000'000'000'000, // EUR 500,000
                 .mUnits = 33'333'333,             // 0.33333333 units, all of the lot
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 49'999'999'000'000, // EUR 499,999.99
                 .mUnits = 100'000'000,            // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-05-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 3},
                               .mEffectiveDate = day("2024-02-01")},
                              {.mEvents = {{StatementEventKind::CorporateAction, 0, 1}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 3},
                               .mEffectiveDate = day("2024-05-01")}});

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    EXPECT_EQ(isin.mAppliedActions.size(), 2U);
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 33'333'333); // 0.33333333
    // The April unit, still EUR 499,999.99: 49,999,999 cents x 10^14 ticks a cent.
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks,
              ExactRational{WideInteger{49'999'999} * 100'000'000'000'000});

    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionQuantityRounded);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mTaxDate, day("2024-02-01"));
    EXPECT_EQ(result.mDiagnostics.at(1).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics.at(1).mMessageKey, MessageKey::CorporateActionQuantityRounded);
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mTaxDate, day("2024-05-01"));
}

TEST(FifoMatcherTest, AReverseSplitThatEmptiesALotIsUnrepresentable) {
    // One lot of 0.00000001 units, 1 / 10: exactly 0.000000001, which rounds to 0. The lot would
    // keep its purchase value with no units, so the action is not applied.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1,                 // 0.00000001 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 10},
                               .mEffectiveDate = day("2024-03-01")}});

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::NumericFailure);
    EXPECT_EQ(isin.mStoppedAt,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 0}));
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 1); // unchanged

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::CorporateActionUnrepresentable);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionUnrepresentable);
}

TEST(FifoMatcherTest, TheSeveralYearsExampleKeepsOlderAndNewerShares) {
    // tax_rules.md, "Example across several years", with prices: 10 bought in 2008, 4 sold in
    // 2023, 8 bought in 2023, 2 / 1 on 2024-02-01: 12 older and 16 newer shares. The sale of 20
    // takes the 12 older and 8 newer, the December sale of 4 more newer; 4 newer remain.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-05-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-09-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 800'000'000,       // 8 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'500'000'000, // EUR 15
                 .mUnits = 2'000'000'000,     // 20 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 900'000'000, // EUR 9
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-02-01")}});

    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& isin = result.mIsins.at(0);

    // The 2023 sale is before the split: 4 units of the 2008 lot, not changed.
    ASSERT_EQ(isin.mEarlierMatches.size(), 1U);
    EXPECT_EQ(isin.mEarlierMatches.at(0).mUnits, 400'000'000);

    ASSERT_EQ(isin.mMatches.size(), 3U);

    const auto& older = isin.mMatches.at(0);

    EXPECT_EQ(older.mPurchaseDate, day("2008-05-10"));
    EXPECT_EQ(older.mUnits, 1'200'000'000);                         // 6 x 2 = 12 units
    EXPECT_EQ(older.mBasis.mTicks, ticks(600'000'000'000'000'000)); // 6 x 10 = EUR 60

    const auto& newer = isin.mMatches.at(1);

    EXPECT_EQ(newer.mPurchaseDate, day("2023-09-01"));
    EXPECT_EQ(newer.mUnits, 800'000'000);                           // 8 of the 16 units
    EXPECT_EQ(newer.mBasis.mTicks, ticks(800'000'000'000'000'000)); // 160 x 8 / 16 = EUR 80

    const auto& december = isin.mMatches.at(2);

    EXPECT_EQ(december.mPurchaseDate, day("2023-09-01"));
    EXPECT_EQ(december.mUnits, 400'000'000);                           // 4 units
    EXPECT_EQ(december.mBasis.mTicks, ticks(400'000'000'000'000'000)); // EUR 40
    EXPECT_EQ(december.mGainTicks, ticks(-40'000'000'000'000'000));    // 36 - 40 = EUR -4

    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 400'000'000); // 4 newer units
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks,
              ticks(400'000'000'000'000'000)); // EUR 40
}

TEST(FifoMatcherTest, ATradeOnTheEffectiveDateIsInNewUnits) {
    // The action comes before the trades of its day: the purchase of 1 unit on the split day is
    // already in new units. 3 units become 6, the 1 unit stays 1.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 100'000'000,     // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-06-01")}});

    const auto& lots = result.mIsins.at(0).mOpenLots;

    ASSERT_EQ(lots.size(), 2U);
    EXPECT_EQ(lots.at(0).mState.mRemainingUnits, 600'000'000); // 3 x 2 = 6
    EXPECT_EQ(lots.at(1).mState.mRemainingUnits, 100'000'000); // 1, not changed
}

TEST(FifoMatcherTest, AnEarlierConfirmedDatePutsTheLaterPurchaseAfterTheSplit) {
    // fifo.md: the row is dated 2024-11-14, the announcement 2024-11-10, and 0.1 units were
    // bought on 2024-11-12. With 2024-11-10 the purchase is after the split: 1 unit becomes 2,
    // the 0.1 stays 0.1.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-11-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 500'000'000, // EUR 5
                 .mUnits = 10'000'000,      // 0.1 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-11-14")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-11-10")}});

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mAppliedActions,
              (std::vector<AppliedCorporateAction>{
                  {.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                   .mEffectiveDate = day("2024-11-10"),
                   .mRatio = {.mNewShares = 2, .mOldShares = 1}}}));
    ASSERT_EQ(isin.mOpenLots.size(), 2U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 200'000'000); // 2 units
    EXPECT_EQ(isin.mOpenLots.at(1).mState.mRemainingUnits, 10'000'000);  // 0.1 units
}

TEST(FifoMatcherTest, TheRowDateAsEffectiveDatePutsTheLaterPurchaseBeforeTheSplit) {
    // The same history with the row date 2024-11-14: the 0.1 units bought on 2024-11-12 are
    // before the split, so 1 unit becomes 2 and 0.1 becomes 0.2.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-11-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 500'000'000, // EUR 5
                 .mUnits = 10'000'000,      // 0.1 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-11-14")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-11-14")}});

    const auto& lots = result.mIsins.at(0).mOpenLots;

    ASSERT_EQ(lots.size(), 2U);
    EXPECT_EQ(lots.at(0).mState.mRemainingUnits, 200'000'000); // 2 units
    EXPECT_EQ(lots.at(1).mState.mRemainingUnits, 20'000'000);  // 0.2 units
}

TEST(FifoMatcherTest, OneActionRowChangesTheLotsOfEveryBrokerOnce) {
    // 2 units at Trade Republic and 1 at Interactive Brokers; only Trade Republic has a split
    // row. The split is an event of the investment, so both lots double, once: 4 and 2 units.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {{.mBroker = Broker::TradeRepublic}}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-10"),
                               .mSources = {{.mBroker = Broker::InteractiveBrokers}}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {{.mBroker = Broker::TradeRepublic}}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-06-01")}});

    const auto& lots = result.mIsins.at(0).mOpenLots;

    ASSERT_EQ(lots.size(), 2U);
    EXPECT_EQ(lots.at(0).mState.mRemainingUnits, 400'000'000); // 2 x 2
    EXPECT_EQ(lots.at(1).mState.mRemainingUnits, 200'000'000); // 1 x 2
}

TEST(FifoMatcherTest, ASplitAfterTheLastSaleChangesOnlyTheLotsAtYearEnd) {
    // 2 units bought, 1 sold in March, split 2 / 1 in November: the March sale uses an unchanged
    // lot, and the remaining 1 unit becomes 2 at 31 December.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-11-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-11-01")}});

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 1U);
    EXPECT_EQ(isin.mMatches.at(0).mUnits, 100'000'000);
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 200'000'000); // 1 x 2
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks,
              ticks(100'000'000'000'000'000)); // still EUR 10
}

TEST(FifoMatcherTest, AnUnconfirmedActionAfterAConfirmedOneStopsWithTheLotsAfterTheFirst) {
    // March 2 / 1 is confirmed, September is not: the pool stops at September with the 3 units
    // doubled to 6.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-10-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit, after the stop
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-09-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        matchYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-03-01")}});

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::CorporateAction);
    EXPECT_EQ(isin.mStoppedAt,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 1}));
    EXPECT_TRUE(isin.mMatches.empty());
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 600'000'000); // 3 x 2
}

TEST(FifoMatcherTest, ASaleWithTooFewUnitsStopsAtASplitRowTenDaysLater) {
    // 10 units, then a sale of 20 on 2024-06-01 and a split row on 2024-06-11, 10 days later. The
    // broker may have booked a split that took effect before the sale, so the pool stops at the
    // split with the lots just before the sale, and no incomplete_history error is given.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 2'000'000'000,   // 20 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-11")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = matchYear(merged, 2024);

    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::CorporateAction);
    EXPECT_EQ(isin.mStoppedAt,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 0}));
    EXPECT_EQ(isin.mShortSale, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_FALSE(isin.mAvailableUnits.has_value());
    EXPECT_TRUE(isin.mMatches.empty());
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 1'000'000'000); // 10 units
}

TEST(FifoMatcherTest, ASplitRowElevenDaysAfterASaleWithTooFewUnitsIsMissingHistory) {
    // The same, with the split row on 2024-06-12, 11 days after the sale: too far for a late
    // booking, so the sale is missing history.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 2'000'000'000,   // 20 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-12")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = matchYear(merged, 2024);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(isin.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(isin.mAvailableUnits, 1'000'000'000); // 10 units
    EXPECT_FALSE(isin.mShortSale.has_value());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
}

TEST(FifoMatcherTest, AMergerRowAfterASaleWithTooFewUnitsIsMissingHistory) {
    // A merger 2 days after the sale is not a split booked late.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 2'000'000'000,   // 20 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03")},
                 .mType = CorporateActionType::Merger},
            },
    }};

    const auto result = matchYear(merged, 2024);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_FALSE(isin.mShortSale.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
}

TEST(FifoMatcherTest, ASaleWithTooFewUnitsOfAnIsinWithoutASaleInTheYearIsMissingHistory) {
    // The sale of 20 and the split row 2 days later are in 2024; the selected year 2025 has no
    // sale of the ISIN, so no question is asked and the sale stays missing history.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 2'000'000'000,   // 20 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = matchYear(merged, 2025);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_FALSE(isin.mShortSale.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
}

TEST(FifoMatcherTest, ASaleWithoutAnyOpenUnitBeforeASplitRowIsMissingHistory) {
    // No purchase before the sale: a split cannot make units out of none.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 2'000'000'000,   // 20 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = matchYear(merged, 2024);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(isin.mAvailableUnits, 0);
    EXPECT_FALSE(isin.mShortSale.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
}

} // namespace
