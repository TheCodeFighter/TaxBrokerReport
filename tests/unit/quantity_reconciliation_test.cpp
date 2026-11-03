#include "processors/corporate_actions.hpp"
#include "processors/loss_eligibility.hpp"
#include "processors/quantity_reconciliation.hpp"
#include "processors/tax_processor.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

SourceReference source(std::size_t aRow, std::string aTransactionId) {
    return SourceReference{
        .mBroker = Broker::TradeRepublic,
        .mFilename = SourceFilename::fromPath("history.csv"),
        .mSourceRow = aRow,
        .mTransactionId = std::move(aTransactionId),
    };
}

SourceReference ibkrSource(std::size_t aRow, std::string aTransactionId) {
    return SourceReference{
        .mBroker = Broker::InteractiveBrokers,
        .mFilename = SourceFilename::fromPath("ibkr.csv"),
        .mSourceRow = aRow,
        .mTransactionId = std::move(aTransactionId),
    };
}

/// The source quantity of one trade: its text, the digits that the import discarded and the exact
/// signed decimal.
std::vector<UnitSourceEvidence> evidence(const SourceReference& aSource,
                                         std::string aText,
                                         std::string aDiscardedDigits,
                                         std::string aCanonicalValue) {
    return {UnitSourceEvidence{
        .mSource = aSource,
        .mSourceText = std::move(aText),
        .mDiscardedDigits = std::move(aDiscardedDigits),
        .mCanonicalValue = std::move(aCanonicalValue),
    }};
}

/// An exact EUR amount: aNumerator / aDenominator.
ExactRational eur(std::int64_t aNumerator, std::int64_t aDenominator) {
    return std::get<ExactRational>(
        ExactRational::create(WideInteger{aNumerator}, WideInteger{aDenominator}));
}

/// A literal number of 10^-16 EUR ticks. One EUR is 10'000'000'000'000'000 ticks.
ExactRational ticks(std::int64_t aTicks) {
    return ExactRational{WideInteger{aTicks}};
}

/// Puts all trades and actions in tax-date order. Equal dates keep the order listed in the test.
void orderChronologically(MergedStatement& aStatement) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    auto& order = aStatement.mChronologicalOrder;

    order.clear();

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

/// Runs the pools of the reporting year without any answer.
CapitalPools poolsOf(StatementMergeResult& aMerged, int aYear) {
    orderChronologically(aMerged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});
    RunDecisions decisions;

    return matchCapitalPools(aMerged.mStatement, prepared.mLedger.value(), decisions);
}

/// A session calculation of the pools with the session's answers. aLatest keeps the pools of the
/// latest calculation, so a test can check them after an answer.
RunCalculation poolsCalculation(StatementMergeResult& aMerged, int aYear, CapitalPools& aLatest) {
    orderChronologically(aMerged.mStatement);

    return [&aMerged, aYear, &aLatest](RunDecisions& aDecisions) {
        const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});

        aLatest = matchCapitalPools(aMerged.mStatement, prepared.mLedger.value(), aDecisions);

        ApplicationResultInput input{.mReportingYear = aYear};
        auto actionDiagnostics = aLatest.mActions.mDiagnostics;
        auto actionRequests = aLatest.mActions.mRequests;
        auto quantityDiagnostics = aLatest.mQuantities.mDiagnostics;
        auto quantityRequests = aLatest.mQuantities.mRequests;

        appendStageOutput(input, std::move(actionDiagnostics), std::move(actionRequests));
        appendStageOutput(input, std::move(quantityDiagnostics), std::move(quantityRequests));

        return input;
    };
}

// An automatic correction

TEST(QuantityReconciliationTest, DiscardedDigitsProveARoundingShortageAndReduceTheSale) {
    // The exact purchases add up to 1.000000002 units, so the sale of exactly 1 unit is covered;
    // the stored purchases add up to 0.99999999, one smallest step short.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(2, "synthetic-buy-1"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-11"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(3, "synthetic-buy-2"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-12"),
                                       .mSources = {source(4, "synthetic-buy-3")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(4, "synthetic-buy-3"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 3'600'000'000, // EUR 36
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence =
                             evidence(source(5, "synthetic-sell-1"), "-1.0000000000", "", "-1")},
                    },
            }};

    const auto pools = poolsOf(merged, 2024);

    ASSERT_EQ(pools.mFifo.mIsins.size(), 1U);

    const auto& pool = pools.mFifo.mIsins.at(0);

    EXPECT_EQ(pool.mStatus, FifoIsinStatus::Matched);
    EXPECT_TRUE(pool.mOpenLots.empty());

    // One correction: the sale goes from 1.00000000 to 0.99999999 units. The impact is
    // 0.00000001 units * EUR 36 = EUR 0.00000036.
    ASSERT_EQ(pool.mCorrections.size(), 1U);
    EXPECT_EQ(pool.mCorrections.at(0).mSale,
              (StatementEventReference{StatementEventKind::Trade, 0, 3}));
    EXPECT_EQ(pool.mCorrections.at(0).mOriginalUnits, 100'000'000);
    EXPECT_EQ(pool.mCorrections.at(0).mCorrectedUnits, 99'999'999);
    EXPECT_EQ(pool.mCorrections.at(0).mKind, QuantityCorrectionKind::RoundingEvidence);
    EXPECT_EQ(pool.mCorrections.at(0).mImpact, eur(36, 100'000'000));
    EXPECT_EQ(pool.mAdjustmentImpact, eur(36, 100'000'000));

    // One warning, no error and no question.
    ASSERT_EQ(pools.mFifo.mDiagnostics.size(), 1U);

    const auto& warning = pools.mFifo.mDiagnostics.at(0);

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, ProcessingDiagnosticCode::QuantityRoundingReconciled);
    EXPECT_EQ(warning.mMessageKey, MessageKey::QuantityRoundingReconciled);
    EXPECT_EQ(warning.mContext.mTaxDate, day("2024-03-01"));
    EXPECT_EQ(warning.mContext.mSources,
              (std::vector<SourceReference>{source(5, "synthetic-sell-1")}));
    EXPECT_EQ(warning.mContext.mIsin, "XX0000000001");
    EXPECT_TRUE(pools.mQuantities.mRequests.empty());
    EXPECT_TRUE(pools.mQuantities.mDiagnostics.empty());

    // The sale takes the three purchases whole: 0.33333333 units each. Each purchase has the
    // basis 0.33333333 * EUR 30 = EUR 9.9999999 = 99'999'999'000'000'000 ticks and the sale value
    // 0.33333333 * EUR 36 = EUR 11.99999988 = 119'999'998'800'000'000 ticks, a gain of
    // EUR 1.99999998.
    ASSERT_EQ(pool.mMatches.size(), 3U);

    for (const auto& match : pool.mMatches)
    {
        EXPECT_EQ(match.mUnits, 33'333'333);
        EXPECT_EQ(match.mBasis.mTicks, ticks(99'999'999'000'000'000));
        EXPECT_EQ(match.mSaleValueTicks, ticks(119'999'998'800'000'000));
        EXPECT_EQ(match.mGainTicks, ticks(19'999'999'800'000'000));
    }

    // The statement of the later stages has the corrected sale; the original keeps 1 unit.
    ASSERT_TRUE(pools.mCorrectedStatement.has_value());
    EXPECT_EQ(
        pools.mCorrectedStatement->mPresentation.mTradeInstruments.at(0).mTransactions.at(3).mUnits,
        99'999'999);
    EXPECT_EQ(merged.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions.at(3).mUnits,
              100'000'000);
    EXPECT_EQ(&pools.statement(merged.mStatement), &*pools.mCorrectedStatement);

    // The original source value stays in the unit evidence of the corrected sale.
    EXPECT_EQ(pools.mCorrectedStatement->mPresentation.mTradeInstruments.at(0)
                  .mTransactions.at(3)
                  .mUnitEvidence.at(0)
                  .mCanonicalValue,
              "-1");
}

TEST(QuantityReconciliationTest, ATradeOfAnotherIsinBetweenThePurchasesAndTheSaleIsIgnored) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share",
            .mIsin = "XX0000000001",
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-01-10"),
                                   .mSources = {source(2, "synthetic-buy-1")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 3'000'000'000, // EUR 30
                     .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                     .mCurrency = Currency::EUR,
                     .mUnitEvidence = evidence(source(2, "synthetic-buy-1"),
                                               "0.3333333340",
                                               "40",
                                               "0.333333334")},
                    {.mMetadata = {.mTaxDate = day("2023-01-11"),
                                   .mSources = {source(3, "synthetic-buy-2")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 3'000'000'000, // EUR 30
                     .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                     .mCurrency = Currency::EUR,
                     .mUnitEvidence = evidence(source(3, "synthetic-buy-2"),
                                               "0.3333333340",
                                               "40",
                                               "0.333333334")},
                    {.mMetadata = {.mTaxDate = day("2023-01-12"),
                                   .mSources = {source(4, "synthetic-buy-3")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 3'000'000'000, // EUR 30
                     .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                     .mCurrency = Currency::EUR,
                     .mUnitEvidence = evidence(source(4, "synthetic-buy-3"),
                                               "0.3333333340",
                                               "40",
                                               "0.333333334")},
                    {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                   .mSources = {source(5, "synthetic-sell-1")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 3'600'000'000, // EUR 36
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR,
                     .mUnitEvidence =
                         evidence(source(5, "synthetic-sell-1"), "-1.0000000000", "", "-1")},
                },
        },
        {
            .mName = "Synthetic Fund",
            .mIsin = "XX0000000002",
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-02-01"),
                                   .mSources = {source(10, "synthetic-fund-buy-1")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto pools = poolsOf(merged, 2024);

    EXPECT_EQ(pools.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(pools.mFifo.mIsins.at(0).mCorrections.size(), 1U);
    EXPECT_EQ(pools.mFifo.mIsins.at(0).mCorrections.at(0).mKind,
              QuantityCorrectionKind::RoundingEvidence);
    EXPECT_TRUE(pools.mFifo.mIsins.at(1).mCorrections.empty());
}

TEST(QuantityReconciliationTest, WithoutACorrectionTheStatementIsTheOriginalOne) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'600'000'000, // EUR 36
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto pools = poolsOf(merged, 2024);

    EXPECT_FALSE(pools.mCorrectedStatement.has_value());
    EXPECT_EQ(&pools.statement(merged.mStatement), &merged.mStatement);
    EXPECT_TRUE(pools.mFifo.mDiagnostics.empty());
    EXPECT_TRUE(pools.mQuantities.mRequests.empty());
    EXPECT_EQ(pools.mFifo.mIsins.at(0).mAdjustmentImpact, ExactRational{});
}

TEST(QuantityReconciliationTest, PurchasesFromTwoBrokersProveTheShortageTogether) {
    // 0.333333334 units at each of Trade Republic, Trade Republic and Interactive Brokers.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(2, "synthetic-buy-1"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-11"),
                                       .mSources = {ibkrSource(7, "synthetic-ibkr-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(ibkrSource(7, "synthetic-ibkr-buy-1"),
                                                   "0.333333334",
                                                   "4",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-12"),
                                       .mSources = {source(4, "synthetic-buy-3")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(4, "synthetic-buy-3"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 3'600'000'000, // EUR 36
                         .mUnits = 100'000'000,
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence =
                             evidence(source(5, "synthetic-sell-1"), "-1.0000000000", "", "-1")},
                    },
            }};

    const auto pools = poolsOf(merged, 2024);
    const auto& pool = pools.mFifo.mIsins.at(0);

    EXPECT_EQ(pool.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(pool.mCorrections.size(), 1U);
    EXPECT_EQ(pool.mCorrections.at(0).mCorrectedUnits, 99'999'999);
    EXPECT_EQ(pool.mCorrections.at(0).mKind, QuantityCorrectionKind::RoundingEvidence);
}

// A question

TEST(QuantityReconciliationTest, WithoutEvidenceTheShortageIsAskedAndTheSaleStaysBlocked) {
    // 0.99999999 units bought, 1 unit sold: 0.00000001 units short, no source digits.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // EUR 30.00
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'600'000'000, // EUR 36.00
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto pools = poolsOf(merged, 2024);
    const auto& pool = pools.mFifo.mIsins.at(0);

    EXPECT_EQ(pool.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(pool.mAvailableUnits, 99'999'999);
    EXPECT_TRUE(pool.mCorrections.empty());
    EXPECT_FALSE(pools.mCorrectedStatement.has_value());

    // The plain error of the FIFO run is replaced by the error with the question linked.
    EXPECT_TRUE(pools.mFifo.mDiagnostics.empty());
    ASSERT_EQ(pools.mQuantities.mDiagnostics.size(), 1U);

    const auto& error = pools.mQuantities.mDiagnostics.at(0);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(error.mMessageKey, MessageKey::IncompleteHistoryCorrectionRequired);
    EXPECT_EQ(error.mContext.mTaxDate, day("2024-03-01"));
    EXPECT_EQ(error.mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-sell-1")}));

    ASSERT_EQ(pools.mQuantities.mRequests.size(), 1U);

    const auto& request = pools.mQuantities.mRequests.at(0);

    EXPECT_EQ(request.mProcessingDiagnostics, (std::vector<std::size_t>{0}));
    EXPECT_EQ(request.mQuestion.mType, RequestType::QuantityCorrectionRequired);
    EXPECT_EQ(request.mQuestion.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(request.mQuestion.mTaxDate, day("2024-03-01"));
    EXPECT_EQ(request.mQuestion.mIsin, "XX0000000001");

    // The removed units are valued at the higher of the sale price (EUR 36) and the lot value
    // (EUR 30), and no earlier rounding used the budget.
    const auto& question = std::get<QuantityCorrectionQuestion>(request.mQuestion.mData);

    EXPECT_EQ(question.mIsin, "XX0000000001");
    EXPECT_EQ(question.mName, "Synthetic Share");
    EXPECT_EQ(question.mSaleDate, day("2024-03-01"));
    EXPECT_EQ(question.mSaleTransaction,
              (TransactionIdentity{Broker::TradeRepublic, "synthetic-sell-1"}));
    EXPECT_EQ(question.mOriginalUnits, 100'000'000);
    EXPECT_EQ(question.mOpenUnits, 99'999'999);
    EXPECT_EQ(question.mUnitValue, eur(36, 1));
    EXPECT_EQ(question.mBudgetUsed, ExactRational{});
}

TEST(QuantityReconciliationTest, ALotWorthMoreThanTheSaleValuesTheRemovedUnits) {
    // The lot costs EUR 40 per unit and the sale brings EUR 36: the higher value counts.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 4'000'000'000, // EUR 40.00
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'600'000'000, // EUR 36.00
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto pools = poolsOf(merged, 2024);

    ASSERT_EQ(pools.mQuantities.mRequests.size(), 1U);
    EXPECT_EQ(
        std::get<QuantityCorrectionQuestion>(pools.mQuantities.mRequests.at(0).mQuestion.mData)
            .mUnitValue,
        eur(40, 1));
}

TEST(QuantityReconciliationTest, ASessionAnswerWithTheProposedQuantityCorrectsTheSale) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // EUR 30.00
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'600'000'000, // EUR 36.00
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};
    CapitalPools latest;
    DecisionSession session;

    session.start(poolsCalculation(merged, 2024, latest));

    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_EQ(session.result().mRequests.at(0).mType, RequestType::QuantityCorrectionRequired);

    const auto requestId = session.result().mRequests.at(0).mId;
    const auto rejections =
        session.answer({{requestId,
                         session.result().mRunRevision,
                         QuantityCorrectionAnswer{.mCorrectedUnits = 99'999'999}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(session.result().mRequests.empty());

    const auto& pool = latest.mFifo.mIsins.at(0);

    EXPECT_EQ(pool.mStatus, FifoIsinStatus::Matched);

    // The answer is the user's assertion, not proven rounding.
    ASSERT_EQ(pool.mCorrections.size(), 1U);
    EXPECT_EQ(pool.mCorrections.at(0).mOriginalUnits, 100'000'000);
    EXPECT_EQ(pool.mCorrections.at(0).mCorrectedUnits, 99'999'999);
    EXPECT_EQ(pool.mCorrections.at(0).mKind, QuantityCorrectionKind::UserConfirmed);
    EXPECT_EQ(pool.mCorrections.at(0).mImpact, eur(36, 100'000'000));

    ASSERT_EQ(latest.mFifo.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mFifo.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(latest.mFifo.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::SourceQuantityCorrected);
    EXPECT_EQ(latest.mFifo.mDiagnostics.at(0).mMessageKey, MessageKey::SourceQuantityCorrected);
    EXPECT_TRUE(latest.mQuantities.mDiagnostics.empty());

    // The sale uses the 0.99999999 units of the purchase: basis 0.99999999 * EUR 30 =
    // EUR 29.9999997, sale value 0.99999999 * EUR 36 = EUR 35.99999964. No lot is created.
    ASSERT_EQ(pool.mMatches.size(), 1U);
    EXPECT_EQ(pool.mMatches.at(0).mUnits, 99'999'999);
    EXPECT_EQ(pool.mMatches.at(0).mBasis.mTicks, ticks(299'999'997'000'000'000));
    EXPECT_EQ(pool.mMatches.at(0).mSaleValueTicks, ticks(359'999'996'400'000'000));
    EXPECT_EQ(pool.mMatches.at(0).mGainTicks, ticks(59'999'999'400'000'000));
    EXPECT_TRUE(pool.mOpenLots.empty());
}

TEST(QuantityReconciliationTest, AnAnswerBelowTheOpenUnitsLeavesTheRestOpenAndCostsMore) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // EUR 30.00
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'600'000'000, // EUR 36.00
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};
    CapitalPools latest;
    DecisionSession session;

    session.start(poolsCalculation(merged, 2024, latest));

    const auto requestId = session.result().mRequests.at(0).mId;
    const auto rejections =
        session.answer({{requestId,
                         session.result().mRunRevision,
                         QuantityCorrectionAnswer{.mCorrectedUnits = 99'999'998}}});

    EXPECT_TRUE(rejections.empty());

    const auto& pool = latest.mFifo.mIsins.at(0);

    // 0.00000002 units removed * EUR 36 = EUR 0.00000072.
    ASSERT_EQ(pool.mCorrections.size(), 1U);
    EXPECT_EQ(pool.mCorrections.at(0).mCorrectedUnits, 99'999'998);
    EXPECT_EQ(pool.mCorrections.at(0).mImpact, eur(72, 100'000'000));

    // 0.00000001 units stay open with their basis of EUR 30 per unit * 0.00000001.
    EXPECT_EQ(pool.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(pool.mOpenLots.size(), 1U);
    EXPECT_EQ(pool.mOpenLots.at(0).mState.mRemainingUnits, 1);
    EXPECT_EQ(pool.mOpenLots.at(0).mState.mRemainingBasis.mTicks, ticks(3'000'000'000));
}

TEST(QuantityReconciliationTest, ACancelledQuestionKeepsTheErrorWithoutARequest) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // EUR 30.00
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'600'000'000, // EUR 36.00
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};
    CapitalPools latest;
    DecisionSession session;

    session.start(poolsCalculation(merged, 2024, latest));

    const auto requestId = session.result().mRequests.at(0).mId;
    const auto rejections =
        session.answer({{requestId, session.result().mRunRevision, CancelAnswer{}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(session.result().mRequests.empty());
    EXPECT_EQ(latest.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_TRUE(latest.mFifo.mDiagnostics.empty());

    ASSERT_EQ(latest.mQuantities.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mQuantities.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(latest.mQuantities.mDiagnostics.at(0).mMessageKey,
              MessageKey::IncompleteHistoryCorrectionCancelled);
    EXPECT_TRUE(latest.mQuantities.mRequests.empty());
}

TEST(QuantityReconciliationTest, AChangedPurchaseIsAnotherQuestionAndTheOldAnswerIsNotUsed) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // EUR 30.00
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'600'000'000, // EUR 36.00
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};
    CapitalPools latest;
    DecisionSession session;

    session.start(poolsCalculation(merged, 2024, latest));

    const auto firstId = session.result().mRequests.at(0).mId;

    EXPECT_TRUE(session
                    .answer({{firstId,
                              session.result().mRunRevision,
                              QuantityCorrectionAnswer{.mCorrectedUnits = 99'999'999}}})
                    .empty());
    EXPECT_TRUE(session.result().mRequests.empty());

    // The purchase now has 0.99999998 units: the sale is 0.00000002 units short.
    merged.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions.at(0).mUnits = 99'999'998;
    session.start(poolsCalculation(merged, 2024, latest));

    ASSERT_EQ(session.result().mRequests.size(), 1U);

    const auto& question =
        std::get<QuantityCorrectionQuestion>(latest.mQuantities.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(question.mOriginalUnits, 100'000'000);
    EXPECT_EQ(question.mOpenUnits, 99'999'998);
    EXPECT_EQ(latest.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_TRUE(latest.mFifo.mIsins.at(0).mCorrections.empty());
}

TEST(QuantityReconciliationTest, ARejectedAnswerChangesNothing) {
    // At EUR 600'000 per unit one smallest step is worth EUR 0.006 and two are worth EUR 0.012.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 60'000'000'000'000, // EUR 600'000.00
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 60'000'000'000'000, // EUR 600'000.00
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};
    CapitalPools latest;
    DecisionSession session;

    session.start(poolsCalculation(merged, 2024, latest));

    ASSERT_EQ(session.result().mRequests.size(), 1U);

    const auto requestId = session.result().mRequests.at(0).mId;
    const auto revision = session.result().mRunRevision;

    // 0 units.
    auto rejections =
        session.answer({{requestId, revision, QuantityCorrectionAnswer{.mCorrectedUnits = 0}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::CorrectionNotPositive);
    EXPECT_EQ(rejections.at(0).mMessageKey, MessageKey::AnswerRejectedCorrectionNotPositive);
    EXPECT_EQ(rejections.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", requestId)}));

    // 1 unit is more than the 0.99999999 units that are open.
    rejections = session.answer(
        {{requestId, revision, QuantityCorrectionAnswer{.mCorrectedUnits = 100'000'000}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::CorrectionAboveOpenUnits);
    EXPECT_EQ(rejections.at(0).mMessageKey, MessageKey::AnswerRejectedCorrectionAboveOpenUnits);

    // 0.99999900 units is 0.00000100 below the sale: the limit is not reached by a smaller one.
    rejections = session.answer(
        {{requestId, revision, QuantityCorrectionAnswer{.mCorrectedUnits = 99'999'900}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::CorrectionTooLarge);
    EXPECT_EQ(rejections.at(0).mMessageKey, MessageKey::AnswerRejectedCorrectionTooLarge);

    // 0.99999998 units removes 0.00000002 units: EUR 0.012, above the budget of EUR 0.01.
    rejections = session.answer(
        {{requestId, revision, QuantityCorrectionAnswer{.mCorrectedUnits = 99'999'998}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::CorrectionOverBudget);
    EXPECT_EQ(rejections.at(0).mMessageKey, MessageKey::AnswerRejectedCorrectionOverBudget);

    // Nothing changed: the same question is open.
    EXPECT_EQ(session.result().mRunRevision, revision);
    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_EQ(session.result().mRequests.at(0).mId, requestId);
}

TEST(QuantityReconciliationTest, AnAnswerOneStepBelowTheLimitIsAccepted) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000'000, // EUR 10'000.00
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'000'000'000'000, // EUR 10'000.00
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};
    CapitalPools latest;
    DecisionSession session;

    session.start(poolsCalculation(merged, 2024, latest));

    ASSERT_EQ(session.result().mRequests.size(), 1U);

    // 0.99999901 units removes 0.00000099 units, one step below the limit of 0.00000100.
    const auto requestId = session.result().mRequests.at(0).mId;
    const auto rejections =
        session.answer({{requestId,
                         session.result().mRunRevision,
                         QuantityCorrectionAnswer{.mCorrectedUnits = 99'999'901}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(session.result().mRequests.empty());

    const auto& pool = latest.mFifo.mIsins.at(0);

    // 0.00000099 units * EUR 10'000 = EUR 0.0099, below EUR 0.01.
    EXPECT_EQ(pool.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(pool.mCorrections.size(), 1U);
    EXPECT_EQ(pool.mCorrections.at(0).mCorrectedUnits, 99'999'901);
    EXPECT_EQ(pool.mCorrections.at(0).mKind, QuantityCorrectionKind::UserConfirmed);
    EXPECT_EQ(pool.mCorrections.at(0).mImpact, eur(99, 10'000));

    ASSERT_EQ(latest.mFifo.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mFifo.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::SourceQuantityCorrected);

    // 0.99999999 - 0.99999901 = 0.00000098 units stay open.
    ASSERT_EQ(pool.mOpenLots.size(), 1U);
    EXPECT_EQ(pool.mOpenLots.at(0).mState.mRemainingUnits, 98);
}

TEST(QuantityReconciliationTest, AnAnswerWhoseImpactEqualsTheBudgetIsRejected) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 50'000'000'000'000, // EUR 500'000.00
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 50'000'000'000'000, // EUR 500'000.00
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};
    CapitalPools latest;
    DecisionSession session;

    session.start(poolsCalculation(merged, 2024, latest));

    // The smallest correction, 0.00000001 units * EUR 500'000 = EUR 0.005, is within the budget,
    // so the question is asked.
    ASSERT_EQ(session.result().mRequests.size(), 1U);

    const auto requestId = session.result().mRequests.at(0).mId;
    const auto revision = session.result().mRunRevision;

    // 0.99999998 units removes 0.00000002 units: 0.00000002 * EUR 500'000 = EUR 0.01 exactly,
    // which is not below the budget.
    const auto rejections = session.answer(
        {{requestId, revision, QuantityCorrectionAnswer{.mCorrectedUnits = 99'999'998}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::CorrectionOverBudget);
    EXPECT_EQ(rejections.at(0).mMessageKey, MessageKey::AnswerRejectedCorrectionOverBudget);

    EXPECT_EQ(session.result().mRunRevision, revision);
    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_EQ(session.result().mRequests.at(0).mId, requestId);
}

TEST(QuantityReconciliationTest, AnAnswerOneStepBelowTheBudgetIsAccepted) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 49'999'999'000'000, // EUR 499'999.99
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 49'999'999'000'000, // EUR 499'999.99
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};
    CapitalPools latest;
    DecisionSession session;

    session.start(poolsCalculation(merged, 2024, latest));

    ASSERT_EQ(session.result().mRequests.size(), 1U);

    const auto requestId = session.result().mRequests.at(0).mId;
    const auto rejections =
        session.answer({{requestId,
                         session.result().mRunRevision,
                         QuantityCorrectionAnswer{.mCorrectedUnits = 99'999'998}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(session.result().mRequests.empty());

    const auto& pool = latest.mFifo.mIsins.at(0);

    // 0.00000002 units * EUR 499'999.99 = EUR 0.0099999998, below EUR 0.01.
    EXPECT_EQ(pool.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(pool.mCorrections.size(), 1U);
    EXPECT_EQ(pool.mCorrections.at(0).mCorrectedUnits, 99'999'998);
    EXPECT_EQ(pool.mCorrections.at(0).mImpact, eur(99'999'998, 10'000'000'000));
    EXPECT_EQ(pool.mAdjustmentImpact, eur(99'999'998, 10'000'000'000));

    // 0.00000001 units stay open.
    ASSERT_EQ(pool.mOpenLots.size(), 1U);
    EXPECT_EQ(pool.mOpenLots.at(0).mState.mRemainingUnits, 1);
}

// The limits

TEST(QuantityReconciliationTest, AShortageOfExactlyTheLimitIsMissingHistory) {
    // 0.99999900 units bought, 1 unit sold: 0.00000100 units short, the limit itself.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // EUR 30.00
                 .mUnits = 99'999'900,        // 0.99999900 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'600'000'000, // EUR 36.00
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto pools = poolsOf(merged, 2024);

    EXPECT_EQ(pools.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_TRUE(pools.mQuantities.mRequests.empty());
    EXPECT_TRUE(pools.mQuantities.mDiagnostics.empty());
    EXPECT_FALSE(pools.mCorrectedStatement.has_value());

    ASSERT_EQ(pools.mFifo.mDiagnostics.size(), 1U);
    EXPECT_EQ(pools.mFifo.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(pools.mFifo.mDiagnostics.at(0).mMessageKey, MessageKey::IncompleteHistory);
}

TEST(QuantityReconciliationTest, AShortageOneStepBelowTheLimitIsAsked) {
    // 0.99999901 units bought, 1 unit sold: 0.00000099 units short.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // EUR 30.00
                 .mUnits = 99'999'901,        // 0.99999901 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'600'000'000, // EUR 36.00
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto pools = poolsOf(merged, 2024);

    ASSERT_EQ(pools.mQuantities.mRequests.size(), 1U);
    EXPECT_EQ(
        std::get<QuantityCorrectionQuestion>(pools.mQuantities.mRequests.at(0).mQuestion.mData)
            .mOpenUnits,
        99'999'901);
    EXPECT_TRUE(pools.mFifo.mDiagnostics.empty());
}

TEST(QuantityReconciliationTest, ASaleWithNothingOpenIsMissingHistory) {
    // 0.0000005 units bought and sold, then 0.0000005 units sold again with nothing open.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 50,                // 0.0000005 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-02-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 50,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(4, "synthetic-sell-2")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 50,
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto pools = poolsOf(merged, 2024);

    EXPECT_EQ(pools.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(pools.mFifo.mIsins.at(0).mAvailableUnits, 0);
    EXPECT_TRUE(pools.mQuantities.mRequests.empty());
    ASSERT_EQ(pools.mFifo.mDiagnostics.size(), 1U);
    EXPECT_EQ(pools.mFifo.mDiagnostics.at(0).mMessageKey, MessageKey::IncompleteHistory);
}

TEST(QuantityReconciliationTest, AnImpactEqualToTheBudgetIsMissingHistory) {
    // One smallest step at EUR 1'000'000 per unit is exactly EUR 0.01: not below the budget.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 100'000'000'000'000, // EUR 1'000'000.00
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 100'000'000'000'000, // EUR 1'000'000.00
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto pools = poolsOf(merged, 2024);

    EXPECT_EQ(pools.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_TRUE(pools.mQuantities.mRequests.empty());
    ASSERT_EQ(pools.mFifo.mDiagnostics.size(), 1U);
    EXPECT_EQ(pools.mFifo.mDiagnostics.at(0).mMessageKey, MessageKey::IncompleteHistory);
}

TEST(QuantityReconciliationTest, AnImpactOneStepBelowTheBudgetIsAsked) {
    // At EUR 999'999.99 per unit one smallest step is EUR 0.0099999999.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 99'999'999'000'000, // EUR 999'999.99
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 99'999'999'000'000, // EUR 999'999.99
                 .mUnits = 100'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto pools = poolsOf(merged, 2024);

    ASSERT_EQ(pools.mQuantities.mRequests.size(), 1U);
    EXPECT_EQ(
        std::get<QuantityCorrectionQuestion>(pools.mQuantities.mRequests.at(0).mQuestion.mData)
            .mUnitValue,
        eur(99'999'999, 100));
}

TEST(QuantityReconciliationTest, TwoSmallCorrectionsOverTheCombinedBudgetAreNotBothAccepted) {
    // Each year has three purchases of 0.333333334 units (stored 0.33333333) and a sale of 1 unit
    // at EUR 600'000 per unit. Each correction is worth EUR 0.006; the two together EUR 0.012.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 60'000'000'000'000, // EUR 600'000
                         .mUnits = 33'333'333,
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(2, "synthetic-buy-1"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-11"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 60'000'000'000'000, // EUR 600'000.00
                         .mUnits = 33'333'333,
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(3, "synthetic-buy-2"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-12"),
                                       .mSources = {source(4, "synthetic-buy-3")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 60'000'000'000'000, // EUR 600'000.00
                         .mUnits = 33'333'333,
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(4, "synthetic-buy-3"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-06-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 60'000'000'000'000, // EUR 600'000.00
                         .mUnits = 100'000'000,
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence =
                             evidence(source(5, "synthetic-sell-1"), "-1.0000000000", "", "-1")},
                        {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                       .mSources = {source(6, "synthetic-buy-4")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 60'000'000'000'000, // EUR 600'000.00
                         .mUnits = 33'333'333,
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(6, "synthetic-buy-4"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2024-01-11"),
                                       .mSources = {source(7, "synthetic-buy-5")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 60'000'000'000'000, // EUR 600'000.00
                         .mUnits = 33'333'333,
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(7, "synthetic-buy-5"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2024-01-12"),
                                       .mSources = {source(8, "synthetic-buy-6")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 60'000'000'000'000, // EUR 600'000.00
                         .mUnits = 33'333'333,
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(8, "synthetic-buy-6"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {source(9, "synthetic-sell-2")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 60'000'000'000'000, // EUR 600'000.00
                         .mUnits = 100'000'000,
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence =
                             evidence(source(9, "synthetic-sell-2"), "-1.0000000000", "", "-1")},
                    },
            }};

    const auto pools = poolsOf(merged, 2024);
    const auto& pool = pools.mFifo.mIsins.at(0);

    // The sale of 2023 is corrected; the sale of 2024 stops the pool.
    EXPECT_EQ(pool.mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(pool.mStoppedAt, (StatementEventReference{StatementEventKind::Trade, 0, 7}));
    ASSERT_EQ(pool.mCorrections.size(), 1U);
    EXPECT_EQ(pool.mCorrections.at(0).mSale,
              (StatementEventReference{StatementEventKind::Trade, 0, 3}));
    EXPECT_EQ(pool.mAdjustmentImpact, eur(6, 1000));

    // The statement has the first correction only.
    ASSERT_TRUE(pools.mCorrectedStatement.has_value());
    EXPECT_EQ(
        pools.mCorrectedStatement->mPresentation.mTradeInstruments.at(0).mTransactions.at(3).mUnits,
        99'999'999);
    EXPECT_EQ(
        pools.mCorrectedStatement->mPresentation.mTradeInstruments.at(0).mTransactions.at(7).mUnits,
        100'000'000);

    // A warning for the first sale and the plain missing-history error for the second.
    ASSERT_EQ(pools.mFifo.mDiagnostics.size(), 2U);
    EXPECT_EQ(pools.mFifo.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::QuantityRoundingReconciled);
    EXPECT_EQ(pools.mFifo.mDiagnostics.at(1).mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(pools.mFifo.mDiagnostics.at(1).mMessageKey, MessageKey::IncompleteHistory);
    EXPECT_TRUE(pools.mQuantities.mRequests.empty());
}

// Corrections and corporate-action rounding share one budget

TEST(QuantityReconciliationTest, ActionRoundingAndACorrectionCountTogetherAgainstTheBudget) {
    // Buy 1 unit at EUR 600'000. The 2 / 3 split gives 0.66666667 units, 1/3 of a smallest step
    // above the exact 0.666666666...: worth 1/(3 * 10^8) units * EUR 600'000 / 0.66666667 units
    // = EUR 200'000 / 66'666'667 = about EUR 0.003. Sale at EUR 900'000: one smallest step is
    // EUR 0.009. Together about EUR 0.012, which is not below EUR 0.01; a signed sum would pass.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 60'000'000'000'000,
                 .mUnits = 100'000'000, // 1 unit
                 .mCurrency = Currency::EUR,
                 .mUnitEvidence = std::vector<UnitSourceEvidence>{}},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 90'000'000'000'000,
                 .mUnits = 66'666'668, // 0.66666668 units
                 .mCurrency = Currency::EUR,
                 .mUnitEvidence = std::vector<UnitSourceEvidence>{}},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000},
            },
    }};

    orderChronologically(merged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({2024, merged});
    const std::vector<ConfirmedCorporateAction> confirmed{
        {.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
         .mRatio = {.mNewShares = 2, .mOldShares = 3},
         .mEffectiveDate = day("2023-06-01")}};
    auto fifo = FifoMatcher::match(merged.mStatement, prepared.mLedger.value(), confirmed);
    RunDecisions decisions;
    const auto result = QuantityReconciler::reconcile(merged.mStatement,
                                                      prepared.mLedger.value(),
                                                      fifo,
                                                      {},
                                                      decisions);

    ASSERT_EQ(fifo.mIsins.size(), 1U);
    EXPECT_EQ(fifo.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(fifo.mIsins.at(0).mAvailableUnits, 66'666'667);
    EXPECT_EQ(fifo.mIsins.at(0).mAdjustmentImpact, eur(200'000, 66'666'667));

    EXPECT_TRUE(result.mNewCorrections.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());

    // The error of the FIFO run stays.
    EXPECT_TRUE(std::any_of(fifo.mDiagnostics.begin(),
                            fifo.mDiagnostics.end(),
                            [](const ProcessingDiagnostic& aDiagnostic) {
                                return aDiagnostic.mMessageKey == MessageKey::IncompleteHistory;
                            }));
}

TEST(QuantityReconciliationTest, ARoundingWithinTheBudgetAsksAndNoDigitsProofFollowsASplit) {
    // Buy 1 unit at EUR 300'000, sale at EUR 450'000. The split rounding is worth about
    // EUR 0.0015 (EUR 100'000 / 66'666'667) and the correction EUR 0.0045: together below
    // EUR 0.01. The exact source units before the split (1) would cover the sale (0.66666668),
    // but they are in the units before the split, so they prove nothing: the user is asked.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 30'000'000'000'000,
                 .mUnits = 100'000'000, // 1 unit
                 .mCurrency = Currency::EUR,
                 .mUnitEvidence = evidence(source(2, "synthetic-buy-1"), "1.0000000000", "", "1")},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 45'000'000'000'000,
                 .mUnits = 66'666'668, // 0.66666668 units
                 .mCurrency = Currency::EUR,
                 .mUnitEvidence =
                     evidence(source(4, "synthetic-sell-1"), "-0.6666666800", "", "-0.66666668")},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000},
            },
    }};

    orderChronologically(merged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({2024, merged});
    const std::vector<ConfirmedCorporateAction> confirmed{
        {.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
         .mRatio = {.mNewShares = 2, .mOldShares = 3},
         .mEffectiveDate = day("2023-06-01")}};
    auto fifo = FifoMatcher::match(merged.mStatement, prepared.mLedger.value(), confirmed);
    RunDecisions decisions;
    const auto result = QuantityReconciler::reconcile(merged.mStatement,
                                                      prepared.mLedger.value(),
                                                      fifo,
                                                      {},
                                                      decisions);

    EXPECT_TRUE(result.mNewCorrections.empty());
    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& question =
        std::get<QuantityCorrectionQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(question.mOriginalUnits, 66'666'668);
    EXPECT_EQ(question.mOpenUnits, 66'666'667);
    EXPECT_EQ(question.mUnitValue, eur(450'000, 1));
    EXPECT_EQ(question.mBudgetUsed, eur(100'000, 66'666'667));
}

// The proof

TEST(QuantityReconciliationTest, ASourceValueThatIsNotADecimalProvesNothing) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence =
                             evidence(source(2, "synthetic-buy-1"), "0.3333333340", "40", "abc")},
                        {.mMetadata = {.mTaxDate = day("2023-01-11"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(3, "synthetic-buy-2"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-12"),
                                       .mSources = {source(4, "synthetic-buy-3")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(4, "synthetic-buy-3"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 3'600'000'000, // EUR 36
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence =
                             evidence(source(5, "synthetic-sell-1"), "-1.0000000000", "", "-1")},
                    },
            }};

    const auto pools = poolsOf(merged, 2024);

    EXPECT_EQ(pools.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(pools.mQuantities.mRequests.size(), 1U);
}

TEST(QuantityReconciliationTest, ASourceValueWithTooManyDecimalsProvesNothing) {
    // 31 fractional digits are more than the exact comparison reads.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(2, "synthetic-buy-1"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.3333333340000000000000000000001")},
                        {.mMetadata = {.mTaxDate = day("2023-01-11"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(3, "synthetic-buy-2"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-12"),
                                       .mSources = {source(4, "synthetic-buy-3")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(4, "synthetic-buy-3"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 3'600'000'000, // EUR 36
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence =
                             evidence(source(5, "synthetic-sell-1"), "-1.0000000000", "", "-1")},
                    },
            }};

    const auto pools = poolsOf(merged, 2024);

    EXPECT_EQ(pools.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(pools.mQuantities.mRequests.size(), 1U);
}

TEST(QuantityReconciliationTest, ASourceValueWithAPointAndNoDigitsAfterItProvesNothing) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence =
                             evidence(source(2, "synthetic-buy-1"), "0.3333333340", "40", "1.")},
                        {.mMetadata = {.mTaxDate = day("2023-01-11"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(3, "synthetic-buy-2"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-12"),
                                       .mSources = {source(4, "synthetic-buy-3")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(4, "synthetic-buy-3"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 3'600'000'000, // EUR 36
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence =
                             evidence(source(5, "synthetic-sell-1"), "-1.0000000000", "", "-1")},
                    },
            }};

    const auto pools = poolsOf(merged, 2024);

    EXPECT_EQ(pools.mQuantities.mRequests.size(), 1U);
}

TEST(QuantityReconciliationTest, ASignedSourceValueIsReadWithoutItsSign) {
    // The purchase value "+0.333333334" and the sale value "-1" are read as 0.333333334 and 1.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(2, "synthetic-buy-1"),
                                                   "0.3333333340",
                                                   "40",
                                                   "+0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-11"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(3, "synthetic-buy-2"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-12"),
                                       .mSources = {source(4, "synthetic-buy-3")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(4, "synthetic-buy-3"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 3'600'000'000, // EUR 36
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence =
                             evidence(source(5, "synthetic-sell-1"), "-1.0000000000", "", "-1")},
                    },
            }};

    const auto pools = poolsOf(merged, 2024);

    EXPECT_EQ(pools.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::Matched);
    EXPECT_EQ(pools.mFifo.mIsins.at(0).mCorrections.size(), 1U);
}

TEST(QuantityReconciliationTest, ExactUnitsThatDoNotCoverTheSaleProveNothing) {
    // The exact purchases add up to 0.99999999, so the shortage is real.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(2, "synthetic-buy-1"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.33333333")},
                        {.mMetadata = {.mTaxDate = day("2023-01-11"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(3, "synthetic-buy-2"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.33333333")},
                        {.mMetadata = {.mTaxDate = day("2023-01-12"),
                                       .mSources = {source(4, "synthetic-buy-3")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(4, "synthetic-buy-3"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.33333333")},
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 3'600'000'000, // EUR 36
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence =
                             evidence(source(5, "synthetic-sell-1"), "-1.0000000000", "", "-1")},
                    },
            }};

    const auto pools = poolsOf(merged, 2024);

    EXPECT_EQ(pools.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(pools.mQuantities.mRequests.size(), 1U);
}

TEST(QuantityReconciliationTest, ACorrectionThatTheMatcherRefusesIsNotCorrectedAgain) {
    // A correction of EUR 0.01 given to the matcher stops the pool as missing history. The stage
    // sees that the sale is corrected and leaves the error.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 100'000'000'000'000, // EUR 1'000'000.00
                 .mUnits = 99'999'999,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 100'000'000'000'000, // EUR 1'000'000.00
                 .mUnits = 99'999'999,              // the sale after its correction
                 .mCurrency = Currency::EUR},
            },
    }};

    orderChronologically(merged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({2024, merged});
    const std::vector<QuantityCorrection> applied{{.mSale = {StatementEventKind::Trade, 0, 1},
                                                   .mOriginalUnits = 100'000'000,
                                                   .mCorrectedUnits = 99'999'999,
                                                   .mKind = QuantityCorrectionKind::UserConfirmed,
                                                   .mImpact = eur(1, 100)}};
    auto fifo = FifoMatcher::match(merged.mStatement, prepared.mLedger.value(), {}, applied);
    RunDecisions decisions;
    const auto result = QuantityReconciler::reconcile(merged.mStatement,
                                                      prepared.mLedger.value(),
                                                      fifo,
                                                      applied,
                                                      decisions);

    EXPECT_EQ(fifo.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(fifo.mIsins.at(0).mAvailableUnits, 99'999'999);
    EXPECT_TRUE(fifo.mIsins.at(0).mCorrections.empty());
    EXPECT_TRUE(result.mNewCorrections.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(QuantityReconciliationTest, TheUnitValueIsTheHigherOfTheSalePriceAndTheLotValue) {
    const std::vector<FifoLot> lots{
        {.mPurchase = {StatementEventKind::Trade, 0, 0},
         .mPurchaseDate = day("2023-01-10"),
         .mState = std::get<LotBasisState>(createLotBasis(3'000'000'000, 100'000'000))}}; // EUR 30
    const TradeTransaction euroSale{.mTradeSide = TradeSide::Sell,
                                    .mUnitPrice = 3'600'000'000, // EUR 36
                                    .mUnits = 100'000'000,
                                    .mCurrency = Currency::EUR};
    const TradeTransaction cheapSale{.mTradeSide = TradeSide::Sell,
                                     .mUnitPrice = 2'000'000'000, // EUR 20
                                     .mUnits = 100'000'000,
                                     .mCurrency = Currency::EUR};
    const TradeTransaction dollarSale{.mTradeSide = TradeSide::Sell,
                                      .mUnitPrice = 3'600'000'000, // USD 36.00
                                      .mUnits = 100'000'000,
                                      .mCurrency = Currency::USD};

    EXPECT_EQ(correctionUnitValue(euroSale, lots), eur(36, 1));
    EXPECT_EQ(correctionUnitValue(cheapSale, lots), eur(30, 1));

    // A sale still in another currency (the conversion stage converts it before FIFO when a rate
    // exists, see trade_conversion_test) has an unknown impact and never reconciles.
    EXPECT_EQ(correctionUnitValue(dollarSale, lots), std::nullopt);
}

TEST(QuantityReconciliationTest, AStatementWithTheInstrumentsInAnotherOrderIsAnInternalError) {
    // FIFO runs on A, B and stops A at its sale (one smallest step short). The stage then gets the
    // statement with B, A: the stopped sale of A points at place 0, which now holds B.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share A",
            .mIsin = "XX0000000001",
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-01-10"),
                                   .mSources = {source(2, "synthetic-buy-1")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 3'000'000'000, // EUR 30
                     .mUnits = 99'999'999,        // 0.99999999 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                   .mSources = {source(3, "synthetic-sell-1")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 3'000'000'000, // EUR 30
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Share B",
            .mIsin = "XX0000000002",
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-01-10"),
                                   .mSources = {source(4, "synthetic-buy-2")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 3'000'000'000, // EUR 30
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                },
        },
    };

    orderChronologically(merged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({2024, merged});
    auto fifo = FifoMatcher::match(merged.mStatement, prepared.mLedger.value());

    ASSERT_EQ(fifo.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    ASSERT_EQ(fifo.mIsins.at(0).mStoppedAt,
              (StatementEventReference{StatementEventKind::Trade, 0, 1}));

    MergedStatement reordered = merged.mStatement;
    RunDecisions decisions;

    std::swap(reordered.mPresentation.mTradeInstruments.at(0),
              reordered.mPresentation.mTradeInstruments.at(1));

    EXPECT_THROW(
        (void)
            QuantityReconciler::reconcile(reordered, prepared.mLedger.value(), fifo, {}, decisions),
        std::logic_error);
}

// The stages after the pools

TEST(QuantityReconciliationTest, TheLossRuleReadsTheCorrectedSale) {
    // Three purchases at EUR 30, a corrected sale of 0.99999999 units at EUR 20 (a loss) and a
    // purchase of 1 unit at EUR 20 on 10 March, inside the 30 days: the replacement blocks the
    // loss of the 0.99999999 sold units.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(2, "synthetic-buy-1"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-11"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(3, "synthetic-buy-2"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2023-01-12"),
                                       .mSources = {source(4, "synthetic-buy-3")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 3'000'000'000, // EUR 30
                         .mUnits = 33'333'333,        // 0.3333333340 stored as 0.33333333
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence = evidence(source(4, "synthetic-buy-3"),
                                                   "0.3333333340",
                                                   "40",
                                                   "0.333333334")},
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 2'000'000'000, // EUR 20
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR,
                         .mUnitEvidence =
                             evidence(source(5, "synthetic-sell-1"), "-1.0000000000", "", "-1")},
                        {.mMetadata = {.mTaxDate = day("2024-03-10"),
                                       .mSources = {source(6, "synthetic-buy-4")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 2'000'000'000, // EUR 20
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                    },
            }};

    orderChronologically(merged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({2024, merged});
    RunDecisions decisions;
    const auto pools = matchCapitalPools(merged.mStatement, prepared.mLedger.value(), decisions);
    const auto loss = LossEligibilityChecker::check(pools.statement(merged.mStatement),
                                                    prepared.mLedger.value(),
                                                    pools.mFifo,
                                                    decisions);

    ASSERT_EQ(loss.mIsins.size(), 1U);
    ASSERT_EQ(loss.mIsins.at(0).mSales.size(), 1U);

    const auto& sale = loss.mIsins.at(0).mSales.at(0);

    EXPECT_EQ(sale.mState, LossSaleState::Decided);
    EXPECT_EQ(sale.mLossUnits, 99'999'999);
    EXPECT_EQ(sale.mKnownIneligibleUnits, 99'999'999);
    ASSERT_EQ(sale.mReplacements.size(), 1U);
    EXPECT_EQ(sale.mReplacements.at(0).mDate, day("2024-03-10"));
    EXPECT_EQ(sale.mReplacements.at(0).mUnits, 99'999'999);
    EXPECT_TRUE(loss.mRequests.empty());
}

} // namespace
