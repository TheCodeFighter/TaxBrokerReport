#include "processors/tax_processor.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <utility>
#include <vector>

namespace {
using namespace taxbroker;

TEST(TaxProcessorTest, RetainsOlderPurchasesSalesAndActionsThroughInclusiveYearEnd) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Unknown,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = *parseCalendarDate("1990-01-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 100'000,
                 .mUnits = 1'000'000'000},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2023-12-31")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 120'000,
                 .mUnits = 200'000'000},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2024-01-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 110'000,
                 .mUnits = 100'000'000},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2024-12-31")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 130'000,
                 .mUnits = 300'000'000},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2025-01-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 140'000,
                 .mUnits = 400'000'000},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2023-06-01")},
                 .mType = CorporateActionType::Split,
                 .mRatio = 200'000'000},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2024-06-01")},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = -100'000'000},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2025-01-02")},
                 .mType = CorporateActionType::ReverseSplit,
                 .mRatio = 10'000'000},
            },
    }};
    merged.mStatement.mChronologicalOrder = {
        {StatementEventKind::Trade, 0, 0},
        {StatementEventKind::CorporateAction, 0, 0},
        {StatementEventKind::Trade, 0, 1},
        {StatementEventKind::Trade, 0, 2},
        {StatementEventKind::CorporateAction, 0, 1},
        {StatementEventKind::Trade, 0, 3},
        {StatementEventKind::Trade, 0, 4},
        {StatementEventKind::CorporateAction, 0, 2},
    };

    const auto result = TaxProcessor{}.prepareReportingYear({2024, merged});

    ASSERT_TRUE(result.mLedger.has_value());

    const auto& ledger = *result.mLedger;

    EXPECT_EQ(ledger.mYear, 2024);
    EXPECT_EQ(ledger.mYearStart, parseCalendarDate("2024-01-01"));
    EXPECT_EQ(ledger.mYearEnd, parseCalendarDate("2024-12-31"));
    EXPECT_EQ(ledger.mHistoricalCapitalEventCount, 3U);
    EXPECT_EQ(ledger.mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::CorporateAction, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
                  {StatementEventKind::CorporateAction, 0, 1},
                  {StatementEventKind::Trade, 0, 3},
              }));
    EXPECT_EQ(ledger.mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 2},
                  {StatementEventKind::CorporateAction, 0, 1},
                  {StatementEventKind::Trade, 0, 3},
              }));
    EXPECT_TRUE(ledger.mHasSelectedYearSales);
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& instrument = result.mMerged.mStatement.mPresentation.mTradeInstruments.at(0);

    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(instrument.mTransactions.at(0).mUnits, 1'000'000'000);
    EXPECT_EQ(instrument.mTransactions.at(1).mTradeSide, TradeSide::Sell);
    EXPECT_EQ(instrument.mTransactions.at(3).mUnits, 300'000'000);
    EXPECT_EQ(instrument.mTransactions.at(4).mUnits, 400'000'000);
    EXPECT_EQ(instrument.mCorporateActions.at(1).mType, CorporateActionType::UnresolvedSplit);
    EXPECT_FALSE(instrument.mCorporateActions.at(1).mRatio.has_value());
    EXPECT_EQ(instrument.mCorporateActions.at(1).mUnitsDelta, -100'000'000);
    EXPECT_EQ(instrument.mCorporateActions.at(2).mRatio, 10'000'000);
    EXPECT_EQ(merged.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions.at(0).mUnits,
              1'000'000'000);
}

TEST(TaxProcessorTest, LossViewIncludesDay30ButExcludesDay31AndRetainsJanuarySalesAndActions) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2023-12-01")},
                 .mTradeSide = TradeSide::Buy},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2023-12-02")},
                 .mTradeSide = TradeSide::Buy},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2024-01-01")},
                 .mTradeSide = TradeSide::Sell},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2024-12-31")},
                 .mTradeSide = TradeSide::Sell},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2025-01-01")},
                 .mTradeSide = TradeSide::Sell},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2025-01-30")},
                 .mTradeSide = TradeSide::Buy},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2025-01-31")},
                 .mTradeSide = TradeSide::Buy},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2023-12-02")},
                 .mType = CorporateActionType::Split,
                 .mRatio = 200'000'000},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2025-01-30")},
                 .mType = CorporateActionType::ReverseSplit,
                 .mRatio = 10'000'000},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2025-01-31")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    merged.mStatement.mChronologicalOrder = {
        {StatementEventKind::Trade, 0, 0},
        {StatementEventKind::CorporateAction, 0, 0},
        {StatementEventKind::Trade, 0, 1},
        {StatementEventKind::Trade, 0, 2},
        {StatementEventKind::Trade, 0, 3},
        {StatementEventKind::Trade, 0, 4},
        {StatementEventKind::CorporateAction, 0, 1},
        {StatementEventKind::Trade, 0, 5},
        {StatementEventKind::CorporateAction, 0, 2},
        {StatementEventKind::Trade, 0, 6},
    };

    const auto result = TaxProcessor{}.prepareReportingYear({2024, std::move(merged)});

    ASSERT_TRUE(result.mLedger.has_value());

    const auto& ledger = *result.mLedger;

    // 1 January - 30 days is 2 December; 31 December + 30 days is 30 January.
    EXPECT_EQ(ledger.mLossWindowStart, parseCalendarDate("2023-12-02"));
    EXPECT_EQ(ledger.mLossWindowEnd, parseCalendarDate("2025-01-30"));
    EXPECT_EQ(ledger.mLossAnalysisEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::CorporateAction, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
                  {StatementEventKind::Trade, 0, 3},
                  {StatementEventKind::Trade, 0, 4},
                  {StatementEventKind::CorporateAction, 0, 1},
                  {StatementEventKind::Trade, 0, 5},
              }));
    EXPECT_EQ(ledger.mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::CorporateAction, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
                  {StatementEventKind::Trade, 0, 3},
              }));
    EXPECT_EQ(ledger.mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 2},
                  {StatementEventKind::Trade, 0, 3},
              }));
    EXPECT_EQ(ledger.mHistoricalCapitalEventCount, 3U);
    EXPECT_EQ(result.mMerged.mStatement.mChronologicalOrder.size(), 10U);
}

TEST(TaxProcessorTest,
     SelectsAllSixEventKindsWithoutAddingIncomeOrUnclassifiedEventsToCapitalLedger) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation = {
        .mTradeInstruments = {{
            .mTransactions = {{.mMetadata = {.mTaxDate = *parseCalendarDate("2024-02-01")},
                               .mTradeSide = TradeSide::Buy}},
            .mCorporateActions = {{.mMetadata = {.mTaxDate = *parseCalendarDate("2024-03-01")}}},
        }},
        .mDividendInstruments =
            {{.mTransactions =
                  {
                      {.mMetadata = {.mTaxDate = *parseCalendarDate("2023-12-31")}},
                      {.mMetadata = {.mTaxDate = *parseCalendarDate("2024-01-01")}},
                      {.mMetadata = {.mTaxDate = *parseCalendarDate("2025-01-01")}},
                  }}},
        .mInterestInstruments =
            {{.mTransactions =
                  {
                      {.mMetadata = {.mTaxDate = *parseCalendarDate("2023-12-31")}},
                      {.mMetadata = {.mTaxDate = *parseCalendarDate("2024-12-31")}},
                      {.mMetadata = {.mTaxDate = *parseCalendarDate("2025-01-01")}},
                  }}},
        .mBenefitEvents = {{.mMetadata = {.mTaxDate = *parseCalendarDate("2024-04-01")}}},
        .mPrivateMarketEvents = {{.mMetadata = {.mTaxDate = *parseCalendarDate("2024-05-01")}}},
    };
    merged.mStatement.mChronologicalOrder = {
        {StatementEventKind::Dividend, 0, 0},
        {StatementEventKind::Interest, 0, 0},
        {StatementEventKind::Dividend, 0, 1},
        {StatementEventKind::Trade, 0, 0},
        {StatementEventKind::CorporateAction, 0, 0},
        {StatementEventKind::Benefit, std::nullopt, 0},
        {StatementEventKind::PrivateMarket, std::nullopt, 0},
        {StatementEventKind::Interest, 0, 1},
        {StatementEventKind::Dividend, 0, 2},
        {StatementEventKind::Interest, 0, 2},
    };

    const auto result = TaxProcessor{}.prepareReportingYear({2024, std::move(merged)});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_EQ(result.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Dividend, 0, 1},
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::CorporateAction, 0, 0},
                  {StatementEventKind::Benefit, std::nullopt, 0},
                  {StatementEventKind::PrivateMarket, std::nullopt, 0},
                  {StatementEventKind::Interest, 0, 1},
              }));
    EXPECT_EQ(result.mLedger->mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::CorporateAction, 0, 0},
              }));
    EXPECT_EQ(result.mLedger->mLossAnalysisEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::CorporateAction, 0, 0},
              }));
    EXPECT_FALSE(result.mLedger->mHasSelectedYearSales);
}

TEST(TaxProcessorTest, TaxDatesControlYearDespiteCrossingSourceAndOrderingTimestamps) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {.mTransactions = {
             {.mMetadata = {.mTaxDate = *parseCalendarDate("2023-12-31"),
                            .mSourceTimestamp = parseSourceTimestamp("2024-01-01T00:00:00Z")},
              .mTradeSide = TradeSide::Sell},
             {.mMetadata = {.mTaxDate = *parseCalendarDate("2024-01-01"),
                            .mSourceTimestamp = parseSourceTimestamp("2023-12-31T23:30:00Z")},
              .mTradeSide = TradeSide::Buy},
             {.mMetadata = {.mTaxDate = *parseCalendarDate("2024-12-31")},
              .mTradeSide = TradeSide::Buy},
             {.mMetadata = {.mTaxDate = *parseCalendarDate("2025-01-01"),
                            .mSourceTimestamp = parseSourceTimestamp("2024-12-31T23:00:00Z"),
                            .mOrderingTimestamp = parseSourceTimestamp("2024-12-31T09:00:00Z")},
              .mTradeSide = TradeSide::Sell},
         }}};
    merged.mStatement.mChronologicalOrder = {
        {StatementEventKind::Trade, 0, 0},
        {StatementEventKind::Trade, 0, 1},
        {StatementEventKind::Trade, 0, 2},
        {StatementEventKind::Trade, 0, 3},
    };

    const auto result = TaxProcessor{}.prepareReportingYear({2024, std::move(merged)});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_EQ(result.mLedger->mHistoricalCapitalEventCount, 1U);
    EXPECT_EQ(result.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_EQ(result.mLedger->mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_FALSE(result.mLedger->mHasSelectedYearSales);
}

TEST(TaxProcessorTest, ValidEmptyInputPreparesEmptyViewsAndExplicitlyHasNoSelectedYearSales) {
    const auto result = TaxProcessor{}.prepareReportingYear({2024, {}});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_TRUE(result.mLedger->mCapitalEvents.empty());
    EXPECT_EQ(result.mLedger->mHistoricalCapitalEventCount, 0U);
    EXPECT_TRUE(result.mLedger->mSelectedYearEvents.empty());
    EXPECT_TRUE(result.mLedger->mLossAnalysisEvents.empty());
    EXPECT_FALSE(result.mLedger->mHasSelectedYearSales);
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mMerged.mDiagnostics.empty());
}

TEST(TaxProcessorTest, PurchasesAndActionsOnlyRetainInventoryCandidatesWithoutInventingSales) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mTransactions = {{.mMetadata = {.mTaxDate = *parseCalendarDate("2024-02-01")},
                           .mTradeSide = TradeSide::Buy}},
        .mCorporateActions = {{.mMetadata = {.mTaxDate = *parseCalendarDate("2024-03-01")},
                               .mType = CorporateActionType::Split,
                               .mRatio = 200'000'000}},
    }};
    merged.mStatement.mChronologicalOrder = {
        {StatementEventKind::Trade, 0, 0},
        {StatementEventKind::CorporateAction, 0, 0},
    };

    const auto result = TaxProcessor{}.prepareReportingYear({2024, std::move(merged)});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_EQ(result.mLedger->mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::CorporateAction, 0, 0},
              }));
    EXPECT_EQ(result.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::CorporateAction, 0, 0},
              }));
    EXPECT_FALSE(result.mLedger->mHasSelectedYearSales);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TaxProcessorTest, RetainsSalePresenceAfterALaterSelectedYearPurchase) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2023-12-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 100'000,
                 .mUnits = 200'000'000},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2024-01-15")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 120'000,
                 .mUnits = 100'000'000},
                {.mMetadata = {.mTaxDate = *parseCalendarDate("2024-02-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 110'000,
                 .mUnits = 100'000'000},
            },
    }};
    merged.mStatement.mChronologicalOrder = {
        {StatementEventKind::Trade, 0, 0},
        {StatementEventKind::Trade, 0, 1},
        {StatementEventKind::Trade, 0, 2},
    };

    const auto result = TaxProcessor{}.prepareReportingYear({2024, std::move(merged)});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_TRUE(result.mLedger->mHasSelectedYearSales);
    EXPECT_EQ(result.mLedger->mHistoricalCapitalEventCount, 1U);
    EXPECT_EQ(result.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_EQ(result.mLedger->mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TaxProcessorTest, FutureOnlySaleIsAnalysisEvidenceWithoutMakingTheSelectedYearNonempty) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {.mTransactions = {
             {.mMetadata = {.mTaxDate = *parseCalendarDate("2025-01-01")},
              .mTradeSide = TradeSide::Sell},
             {.mMetadata = {.mTaxDate = *parseCalendarDate("2025-02-01")},
              .mTradeSide = TradeSide::Buy},
         }}};
    merged.mStatement.mChronologicalOrder = {
        {StatementEventKind::Trade, 0, 0},
        {StatementEventKind::Trade, 0, 1},
    };

    const auto result = TaxProcessor{}.prepareReportingYear({2024, std::move(merged)});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_TRUE(result.mLedger->mCapitalEvents.empty());
    EXPECT_TRUE(result.mLedger->mSelectedYearEvents.empty());
    EXPECT_EQ(result.mLedger->mLossAnalysisEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
              }));
    EXPECT_FALSE(result.mLedger->mHasSelectedYearSales);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TaxProcessorTest, HistoricalOnlySalesRemainHistoryAndDoNotRequireAnInYearCapitalReport) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {.mTransactions = {
             {.mMetadata = {.mTaxDate = *parseCalendarDate("2020-01-01")},
              .mTradeSide = TradeSide::Buy},
             {.mMetadata = {.mTaxDate = *parseCalendarDate("2023-12-31")},
              .mTradeSide = TradeSide::Sell},
         }}};
    merged.mStatement.mChronologicalOrder = {
        {StatementEventKind::Trade, 0, 0},
        {StatementEventKind::Trade, 0, 1},
    };

    const auto result = TaxProcessor{}.prepareReportingYear({2024, std::move(merged)});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_EQ(result.mLedger->mHistoricalCapitalEventCount, 2U);
    EXPECT_EQ(result.mLedger->mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
              }));
    EXPECT_TRUE(result.mLedger->mSelectedYearEvents.empty());
    EXPECT_FALSE(result.mLedger->mHasSelectedYearSales);
}

TEST(TaxProcessorTest, DifferentYearIncludesLeapDayAndRebuildsWithoutLeakingEarlierRunState) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {.mTransactions = {
             {.mMetadata = {.mTaxDate = *parseCalendarDate("2019-12-31")},
              .mTradeSide = TradeSide::Buy},
             {.mMetadata = {.mTaxDate = *parseCalendarDate("2020-02-29")},
              .mTradeSide = TradeSide::Sell},
             {.mMetadata = {.mTaxDate = *parseCalendarDate("2021-01-01")},
              .mTradeSide = TradeSide::Buy},
         }}};
    merged.mStatement.mChronologicalOrder = {
        {StatementEventKind::Trade, 0, 0},
        {StatementEventKind::Trade, 0, 1},
        {StatementEventKind::Trade, 0, 2},
    };
    const TaxProcessor processor;

    const auto first = processor.prepareReportingYear({2020, merged});
    const auto next = processor.prepareReportingYear({2021, merged});
    const auto repeated = processor.prepareReportingYear({2020, merged});
    const auto empty = processor.prepareReportingYear({2024, {}});

    ASSERT_TRUE(first.mLedger.has_value());
    ASSERT_TRUE(next.mLedger.has_value());
    ASSERT_TRUE(repeated.mLedger.has_value());
    ASSERT_TRUE(empty.mLedger.has_value());

    EXPECT_EQ(first.mLedger->mYearStart, parseCalendarDate("2020-01-01"));
    EXPECT_EQ(first.mLedger->mYearEnd, parseCalendarDate("2020-12-31"));
    EXPECT_EQ(first.mLedger->mHistoricalCapitalEventCount, 1U);
    EXPECT_EQ(first.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 1},
              }));
    EXPECT_TRUE(first.mLedger->mHasSelectedYearSales);
    EXPECT_EQ(next.mLedger->mHistoricalCapitalEventCount, 2U);
    EXPECT_EQ(next.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_FALSE(next.mLedger->mHasSelectedYearSales);
    EXPECT_EQ(repeated.mLedger->mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
              }));
    EXPECT_EQ(repeated.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 1},
              }));
    EXPECT_TRUE(empty.mLedger->mCapitalEvents.empty());
    EXPECT_FALSE(empty.mLedger->mHasSelectedYearSales);
    EXPECT_EQ(merged.mStatement.mChronologicalOrder.size(), 3U);
}

TEST(TaxProcessorTest, EarliestSupportedYearUsesSafeDayArithmeticForThePrecedingLossWindow) {
    const auto result = TaxProcessor{}.prepareReportingYear({kMinimumReportingYear, {}});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_EQ(result.mLedger->mYearStart, parseCalendarDate("2000-01-01"));
    EXPECT_EQ(result.mLedger->mYearEnd, parseCalendarDate("2000-12-31"));
    EXPECT_EQ(result.mLedger->mLossWindowStart,
              Date{std::chrono::sys_days{std::chrono::year{1999} / 12 / 2}.time_since_epoch()});
    EXPECT_EQ(result.mLedger->mLossWindowEnd, parseCalendarDate("2001-01-30"));
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TaxProcessorTest, LatestYearUsesSafeDayArithmeticForTheFollowingLossWindow) {
    const auto result = TaxProcessor{}.prepareReportingYear({kMaximumReportingYear, {}});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_EQ(result.mLedger->mYearStart, parseCalendarDate("9999-01-01"));
    EXPECT_EQ(result.mLedger->mYearEnd, parseCalendarDate("9999-12-31"));
    EXPECT_EQ(result.mLedger->mLossWindowStart, parseCalendarDate("9998-12-02"));
    EXPECT_EQ(result.mLedger->mLossWindowEnd,
              Date{std::chrono::sys_days{std::chrono::year{10000} / 1 / 30}.time_since_epoch()});
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TaxProcessorTest, RejectsMissingYearBeforePreparingAnyViews) {
    const auto result = TaxProcessor{}.prepareReportingYear({std::nullopt, {}});

    EXPECT_FALSE(result.mLedger.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::InvalidReportingYear);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::HistoricalValidation);
    EXPECT_EQ(diagnostic.mContext.mField, "reporting_year");
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector{
                  ReportType::CapitalGains,
                  ReportType::Dividends,
                  ReportType::BankDepositInterest,
                  ReportType::OtherInterest,
              }));
    EXPECT_TRUE(diagnostic.mContext.mSources.empty());
    EXPECT_FALSE(diagnostic.mContext.mTaxDate.has_value());
    EXPECT_FALSE(diagnostic.mContext.mIsin.has_value());
    EXPECT_FALSE(diagnostic.mContext.mInstrumentName.has_value());
    EXPECT_EQ(diagnostic.mMessage, "Select a reporting year from 2000 to 9999.");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidReportingYear);
    EXPECT_EQ(
        diagnostic.mMessageParams,
        (std::vector<MessageParam>{numberParam("minYear", 2000U), numberParam("maxYear", 9999U)}));
}

TEST(TaxProcessorTest, RejectsZeroYearBeforePreparingAnyViews) {
    const auto result = TaxProcessor{}.prepareReportingYear({0, {}});

    EXPECT_FALSE(result.mLedger.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::InvalidReportingYear);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::HistoricalValidation);
    EXPECT_EQ(diagnostic.mContext.mField, "reporting_year");
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector{
                  ReportType::CapitalGains,
                  ReportType::Dividends,
                  ReportType::BankDepositInterest,
                  ReportType::OtherInterest,
              }));
    EXPECT_TRUE(diagnostic.mContext.mSources.empty());
    EXPECT_FALSE(diagnostic.mContext.mTaxDate.has_value());
    EXPECT_FALSE(diagnostic.mContext.mIsin.has_value());
    EXPECT_FALSE(diagnostic.mContext.mInstrumentName.has_value());
    EXPECT_EQ(diagnostic.mMessage, "Select a reporting year from 2000 to 9999.");
}

TEST(TaxProcessorTest, RejectsYearBeforeEarliestSupportedYear) {
    const auto result = TaxProcessor{}.prepareReportingYear({kMinimumReportingYear - 1, {}});

    EXPECT_FALSE(result.mLedger.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, ProcessingDiagnosticCode::InvalidReportingYear);
    EXPECT_EQ(result.mDiagnostics.front().mMessage, "Select a reporting year from 2000 to 9999.");
}

TEST(TaxProcessorTest, RejectsNegativeYearBeforePreparingAnyViews) {
    const auto result = TaxProcessor{}.prepareReportingYear({-1, {}});

    EXPECT_FALSE(result.mLedger.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::InvalidReportingYear);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::HistoricalValidation);
    EXPECT_EQ(diagnostic.mContext.mField, "reporting_year");
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector{
                  ReportType::CapitalGains,
                  ReportType::Dividends,
                  ReportType::BankDepositInterest,
                  ReportType::OtherInterest,
              }));
    EXPECT_TRUE(diagnostic.mContext.mSources.empty());
    EXPECT_FALSE(diagnostic.mContext.mTaxDate.has_value());
    EXPECT_FALSE(diagnostic.mContext.mIsin.has_value());
    EXPECT_FALSE(diagnostic.mContext.mInstrumentName.has_value());
    EXPECT_EQ(diagnostic.mMessage, "Select a reporting year from 2000 to 9999.");
}

TEST(TaxProcessorTest, RejectsYearBeyondFourDigitsBeforePreparingAnyViews) {
    const auto result = TaxProcessor{}.prepareReportingYear({10000, {}});

    EXPECT_FALSE(result.mLedger.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::InvalidReportingYear);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::HistoricalValidation);
    EXPECT_EQ(diagnostic.mContext.mField, "reporting_year");
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector{
                  ReportType::CapitalGains,
                  ReportType::Dividends,
                  ReportType::BankDepositInterest,
                  ReportType::OtherInterest,
              }));
    EXPECT_TRUE(diagnostic.mContext.mSources.empty());
    EXPECT_FALSE(diagnostic.mContext.mTaxDate.has_value());
    EXPECT_FALSE(diagnostic.mContext.mIsin.has_value());
    EXPECT_FALSE(diagnostic.mContext.mInstrumentName.has_value());
    EXPECT_EQ(diagnostic.mMessage, "Select a reporting year from 2000 to 9999.");
}

TEST(TaxProcessorTest, RejectsLargeIntegerBeforeChronoNarrowing) {
    const auto result = TaxProcessor{}.prepareReportingYear({2'147'483'647, {}});

    EXPECT_FALSE(result.mLedger.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::InvalidReportingYear);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::HistoricalValidation);
    EXPECT_EQ(diagnostic.mContext.mField, "reporting_year");
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector{
                  ReportType::CapitalGains,
                  ReportType::Dividends,
                  ReportType::BankDepositInterest,
                  ReportType::OtherInterest,
              }));
    EXPECT_TRUE(diagnostic.mContext.mSources.empty());
    EXPECT_FALSE(diagnostic.mContext.mTaxDate.has_value());
    EXPECT_FALSE(diagnostic.mContext.mIsin.has_value());
    EXPECT_FALSE(diagnostic.mContext.mInstrumentName.has_value());
    EXPECT_EQ(diagnostic.mMessage, "Select a reporting year from 2000 to 9999.");
}

TEST(TaxProcessorTest, InvalidYearPreservesUpstreamDataAndDiagnosticsWithoutReadingReferences) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {.mTransactions = {
             {.mMetadata = {.mTaxDate = *parseCalendarDate("2024-01-01")},
              .mTradeSide = TradeSide::Sell,
              .mUnits = 100'000'000},
         }}};
    // Intentionally invalid internal reference proves year validation happens before traversal.
    merged.mStatement.mChronologicalOrder = {{StatementEventKind::Trade, 99, 99}};
    merged.mDiagnostics = {SourcedParseDiagnostic{
        .mSourceIndex = 2,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic = {.mCode = DiagnosticCode::MissingField,
                        .mSourceFile = "synthetic.csv",
                        .mRowIndex = 7,
                        .mField = "datetime",
                        .mMessage = "Missing timestamp."},
    }};

    const auto result = TaxProcessor{}.prepareReportingYear({std::nullopt, std::move(merged)});

    EXPECT_FALSE(result.mLedger.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics.front().mCode, ProcessingDiagnosticCode::InvalidReportingYear);
    EXPECT_EQ(
        result.mMerged.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions.at(0).mUnits,
        100'000'000);
    EXPECT_EQ(result.mMerged.mStatement.mChronologicalOrder,
              (std::vector<StatementEventReference>{{StatementEventKind::Trade, 99, 99}}));
    ASSERT_EQ(result.mMerged.mDiagnostics.size(), 1U);

    const auto& diagnostic = std::get<SourcedParseDiagnostic>(result.mMerged.mDiagnostics.front());

    EXPECT_EQ(diagnostic.mSourceIndex, 2U);
    EXPECT_EQ(diagnostic.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(diagnostic.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(diagnostic.mDiagnostic.mSourceFile, "synthetic.csv");
    EXPECT_EQ(diagnostic.mDiagnostic.mRowIndex, 7U);
    EXPECT_EQ(diagnostic.mDiagnostic.mField, "datetime");
}

TEST(TaxProcessorTest, EmptyPreparedYearRetainsBlockingUpstreamProblemsForLaterReportStatus) {
    StatementMergeResult merged;
    merged.mDiagnostics = {MergeDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Conflicting synthetic trade.",
        .mTaxDate = *parseCalendarDate("2024-12-31"),
        .mIsin = "XX0000000001",
        .mEventKinds = {StatementEventKind::Trade},
    }};

    const auto result = TaxProcessor{}.prepareReportingYear({2024, std::move(merged)});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_TRUE(result.mLedger->mSelectedYearEvents.empty());
    EXPECT_FALSE(result.mLedger->mHasSelectedYearSales);
    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mMerged.mDiagnostics.size(), 1U);

    const auto& diagnostic = std::get<MergeDiagnostic>(result.mMerged.mDiagnostics.front());

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(diagnostic.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mEventKinds, (std::vector{StatementEventKind::Trade}));
}

TEST(TaxProcessorTest, CrossBrokerReferencesPreserveGlobalChronologyAndAllInstrumentIdentities) {
    const SourceReference first{
        .mBroker = Broker::TradeRepublic,
        .mFilename = SourceFilename::fromPath("first.csv"),
        .mSourceRow = 2,
        .mTransactionId = "synthetic-purchase",
        .mInputSequence = {0, 0},
    };
    const SourceReference second{
        .mBroker = Broker::InteractiveBrokers,
        .mFilename = SourceFilename::fromPath("second.csv"),
        .mSourceRow = 2,
        .mTransactionId = "synthetic-sale",
        .mInputSequence = {1, 0},
    };
    const SourceReference otherSource{
        .mBroker = Broker::InteractiveBrokers,
        .mFilename = SourceFilename::fromPath("second.csv"),
        .mSourceRow = 3,
        .mTransactionId = "synthetic-other",
        .mInputSequence = {1, 1},
    };
    const TradeInstrument purchase{
        .mName = "Synthetic First",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {{
            .mMetadata = {.mTaxDate = *parseCalendarDate("2023-12-31"), .mSources = {first}},
            .mTradeSide = TradeSide::Buy,
            .mUnits = 200'000'000,
        }},
    };
    const TradeInstrument sale{
        .mName = "Synthetic First",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {{
            .mMetadata = {.mTaxDate = *parseCalendarDate("2024-01-01"), .mSources = {second}},
            .mTradeSide = TradeSide::Sell,
            .mUnits = 100'000'000,
        }},
    };
    const TradeInstrument otherInstrument{
        .mName = "Synthetic Other",
        .mIsin = "XX0000000002",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {{
            .mMetadata = {.mTaxDate = *parseCalendarDate("2025-01-01"), .mSources = {otherSource}},
            .mTradeSide = TradeSide::Buy,
            .mUnits = 300'000'000,
        }},
    };
    const ParseResult firstParsed{
        .mBroker = Broker::TradeRepublic,
        .mStatement = {.mTradeInstruments = {purchase}},
    };
    const ParseResult secondParsed{
        .mBroker = Broker::InteractiveBrokers,
        .mStatement = {.mTradeInstruments = {sale, otherInstrument}},
    };
    const std::vector<StatementMergeInput> inputs{{0, firstParsed}, {1, secondParsed}};

    auto result =
        TaxProcessor{}.prepareReportingYear({2024, DeterministicStatementMerger{}.merge(inputs)});
    auto moved = std::move(result);

    ASSERT_TRUE(moved.mLedger.has_value());

    EXPECT_EQ(moved.mLedger->mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
              }));
    EXPECT_EQ(moved.mLedger->mHistoricalCapitalEventCount, 1U);
    EXPECT_EQ(moved.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 1},
              }));
    EXPECT_EQ(moved.mLedger->mLossAnalysisEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 1, 0},
              }));
    EXPECT_TRUE(moved.mLedger->mHasSelectedYearSales);
    ASSERT_EQ(moved.mMerged.mStatement.mPresentation.mTradeInstruments.size(), 2U);

    const auto& firstInstrument = moved.mMerged.mStatement.mPresentation.mTradeInstruments.at(0);

    EXPECT_EQ(firstInstrument.mIsin, "XX0000000001");
    EXPECT_EQ(firstInstrument.mTransactions.at(0).mMetadata.mSources, (std::vector{first}));
    EXPECT_EQ(firstInstrument.mTransactions.at(1).mMetadata.mSources, (std::vector{second}));
    EXPECT_EQ(firstInstrument.mTransactions.at(0).mUnits, 200'000'000);
    EXPECT_EQ(firstInstrument.mTransactions.at(1).mUnits, 100'000'000);
    EXPECT_EQ(moved.mMerged.mStatement.mPresentation.mTradeInstruments.at(1).mIsin, "XX0000000002");
    EXPECT_TRUE(moved.mDiagnostics.empty());
    EXPECT_TRUE(moved.mMerged.mDiagnostics.empty());
}

} // namespace
