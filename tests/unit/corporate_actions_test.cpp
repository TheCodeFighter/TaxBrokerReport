#include "processors/corporate_actions.hpp"
#include "processors/tax_processor.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
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

/// The exact source text of a Trade Republic `shares` value, as the parser keeps it.
std::vector<UnitSourceEvidence> sharesEvidence(const SourceReference& aSource,
                                               const std::string& aShares) {
    return {UnitSourceEvidence{
        .mSource = aSource,
        .mSourceText = aShares,
        .mCanonicalValue = aShares,
    }};
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

/// Prepares the real reporting-year ledger, runs the matcher and resolves the corporate actions
/// without any answer.
CorporateActionResult resolveYear(StatementMergeResult& aMerged, int aYear) {
    orderChronologically(aMerged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});
    const auto& ledger = prepared.mLedger.value();
    const auto fifo = FifoMatcher::match(aMerged.mStatement, ledger);
    RunDecisions decisions;

    return CorporateActionResolver::resolve(aMerged.mStatement, ledger, fifo, decisions);
}

/// A session calculation that resolves the corporate actions with the session's answers.
/// aLatest keeps the stage result of the latest calculation, so a test can check it after an
/// answer.
RunCalculation
resolvingCalculation(StatementMergeResult& aMerged, int aYear, CorporateActionResult& aLatest) {
    orderChronologically(aMerged.mStatement);

    return [&aMerged, aYear, &aLatest](RunDecisions& aDecisions) {
        const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});
        const auto& ledger = prepared.mLedger.value();
        const auto fifo = FifoMatcher::match(aMerged.mStatement, ledger);

        aLatest = CorporateActionResolver::resolve(aMerged.mStatement, ledger, fifo, aDecisions);

        ApplicationResultInput input{.mReportingYear = aYear};
        auto diagnostics = aLatest.mDiagnostics;
        auto requests = aLatest.mRequests;

        appendStageOutput(input, std::move(diagnostics), std::move(requests));

        return input;
    };
}

TEST(CorporateActionResolverTest, SplitBeforeTheSaleAsksForTheRatio) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-15"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000, // 1 unit
                 .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "1")},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);

    const auto& action = result.mActions.at(0);

    EXPECT_EQ(action.mIsin, "XX0000000001");
    EXPECT_EQ(action.mName, "Synthetic Share");
    EXPECT_EQ(action.mEffectiveDate, day("2023-01-15"));
    EXPECT_EQ(action.mEvents,
              (std::vector<StatementEventReference>{{StatementEventKind::CorporateAction, 0, 0}}));
    EXPECT_EQ(action.mState, CorporateActionState::WaitingForRatio);
    EXPECT_FALSE(action.mRatio.has_value());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionAmbiguous);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionAmbiguousRatioRequired);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::CorporateAction);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Synthetic Share");
    // The date and source of the split, not of the sale.
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2023-01-15"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-split-1")}));

    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& request = result.mRequests.at(0);
    const auto& question = request.mQuestion;

    EXPECT_EQ(request.mProcessingDiagnostics, (std::vector<std::size_t>{0}));
    EXPECT_EQ(question.mType, RequestType::CorporateActionRatioRequired);
    EXPECT_EQ(question.mAffectedReports, (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(question.mSources, (std::vector<SourceReference>{source(3, "synthetic-split-1")}));
    EXPECT_EQ(question.mTaxDate, day("2023-01-15"));
    EXPECT_EQ(question.mIsin, "XX0000000001");

    const auto& data = std::get<CorporateActionRatioQuestion>(question.mData);

    EXPECT_EQ(data.mIsin, "XX0000000001");
    EXPECT_EQ(data.mName, "Synthetic Share");
    EXPECT_EQ(data.mEffectiveDate, day("2023-01-15"));
    // The `shares` value is shown as it is; it is not a ratio.
    EXPECT_EQ(data.mSources,
              (std::vector<CorporateActionSourceFacts>{{.mBroker = Broker::TradeRepublic,
                                                        .mTransactionId = "synthetic-split-1",
                                                        .mSourceValue = "1"}}));
    EXPECT_EQ(data.mPositionBefore, 100'000'000); // 1 unit bought before the split
    // The day after the purchase of 2022-01-10, up to the sale on 2024-03-10: any date between
    // keeps the 1 unit before the split.
    EXPECT_EQ(data.mEarliestDate, day("2022-01-11"));
    EXPECT_EQ(data.mLatestDate, day("2024-03-10"));
}

TEST(CorporateActionResolverTest, SplitAfterTheSaleInTheSameYearAsksForTheRatio) {
    // The split is in the selected year, after the sale: still up to 31 December.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-09-01"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000, // 1 unit
                 .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-1"), "1")},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionAmbiguous);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionAmbiguousRatioRequired);
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    // The date and source of the split, not of the sale.
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-09-01"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(4, "synthetic-split-1")}));

    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mPositionBefore, 100'000'000); // 2 units bought - 1 unit sold
}

TEST(CorporateActionResolverTest, ASplitWithoutASaleThisYearIsNotAsked) {
    // Nothing of this investment is reported for 2024, so the split blocks nothing.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-15"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000, // 1 unit
                 .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "1")},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    EXPECT_TRUE(result.mActions.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(CorporateActionResolverTest, ASplitOfAnInvestmentSoldOnlyInOtherYearsIsNotAsked) {
    // Sales in 2023 and 2025, none in 2024: nothing of this investment is reported for 2024.
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
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 300'000'000,       // 3 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2023-12-31"),
                                       .mSources = {source(3, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 2'000'000'000, // EUR 20
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2025-01-01"),
                                       .mSources = {source(5, "synthetic-sell-2")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 2'000'000'000, // EUR 20
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {source(4, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 200'000'000, // 2 units
                         .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-1"), "2")},
                    },
            }};

    const auto result = resolveYear(merged, 2024);

    EXPECT_TRUE(result.mActions.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(CorporateActionResolverTest, ARowWithoutATransactionIdHasNoIdInTheQuestion) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 300'000'000,       // 3 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                       .mSources = {source(4, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {SourceReference{
                                           .mBroker = Broker::TradeRepublic,
                                           .mFilename = SourceFilename::fromPath("history.csv"),
                                           .mSourceRow = 3,
                                       }}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 300'000'000, // 3 units
                         .mUnitEvidence =
                             {
                                 UnitSourceEvidence{
                                     .mSource =
                                         SourceReference{
                                             .mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("history.csv"),
                                             .mSourceRow = 3,
                                         },
                                     .mSourceText = "3",
                                     .mCanonicalValue = "3",
                                 }}},
                    },
            }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mSources,
              (std::vector<CorporateActionSourceFacts>{
                  {.mBroker = Broker::TradeRepublic, .mSourceValue = "3"}}));
}

TEST(CorporateActionResolverTest, ASplitAfter31DecemberIsNotAsked) {
    // The 2025 split is not in the 2024 ledger, so the pool does not stop.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-15"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000, // 1 unit
                 .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-1"), "1")},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    EXPECT_TRUE(result.mActions.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(CorporateActionResolverTest, ACryptoSplitIsNotAsked) {
    // The gates block a crypto sale anyway, so a ratio answer could not lead to a report.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Coin",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Crypto,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-15"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000, // 1 unit
                 .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "1")},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::NotAskedForClass);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionNotApplied);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionNotApplied);
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2023-06-15"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-split-1")}));

    EXPECT_TRUE(result.mRequests.empty());
}

TEST(CorporateActionResolverTest, NoUnitsBeforeTheSplitIsNotAsked) {
    // 2 units bought and 2 sold before the split: there is nothing to split.
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
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 200'000'000,       // 2 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2023-03-01"),
                                       .mSources = {source(3, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 2'000'000'000, // EUR 20
                         .mUnits = 200'000'000,       // 2 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                       .mSources = {source(5, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-03-10"),
                                       .mSources = {source(6, "synthetic-sell-2")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 2'000'000'000, // EUR 20
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-06-01"),
                                       .mSources = {source(4, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 100'000'000, // 1 unit
                         .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-1"), "1")},
                    },
            }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::NoPosition);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::CorporateActionNotApplied);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionNotApplied);

    EXPECT_TRUE(result.mRequests.empty());
}

TEST(CorporateActionResolverTest, OneActionAtTwoBrokersIsOneQuestion) {
    // 2 units at Trade Republic and 1 at Interactive Brokers; both report the split on
    // 2024-06-01. One action, one question for the whole 3 units.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 200'000'000,       // 2 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-02-10"),
                                       .mSources = {ibkrSource(2, "synthetic-ibkr-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                       .mSources = {source(4, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {source(3, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 200'000'000, // 2 units
                         .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "2")},
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {ibkrSource(3, "synthetic-ibkr-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                    },
            }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);

    const auto& action = result.mActions.at(0);

    EXPECT_EQ(action.mState, CorporateActionState::WaitingForRatio);
    EXPECT_EQ(action.mEvents,
              (std::vector<StatementEventReference>{{StatementEventKind::CorporateAction, 0, 0},
                                                    {StatementEventKind::CorporateAction, 0, 1}}));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAmbiguousRatioRequired);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-split-1"),
                                            ibkrSource(3, "synthetic-ibkr-split-1")}));

    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& question = result.mRequests.at(0).mQuestion;
    const auto& data = std::get<CorporateActionRatioQuestion>(question.mData);

    EXPECT_EQ(question.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-split-1"),
                                            ibkrSource(3, "synthetic-ibkr-split-1")}));
    // Interactive Brokers has no quantity field here.
    EXPECT_EQ(
        data.mSources,
        (std::vector<CorporateActionSourceFacts>{
            {.mBroker = Broker::TradeRepublic,
             .mTransactionId = "synthetic-split-1",
             .mSourceValue = "2"},
            {.mBroker = Broker::InteractiveBrokers, .mTransactionId = "synthetic-ibkr-split-1"}}));
    EXPECT_EQ(data.mPositionBefore, 300'000'000); // 2 + 1 units across both brokers
}

TEST(CorporateActionResolverTest, TwoRowsOfOneBrokerOnOneDayAreAmbiguous) {
    // One split reported twice, or two splits? And in which order? The files cannot tell.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 300'000'000,       // 3 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {source(3, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 300'000'000, // 3 units
                         .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "3")},
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {source(4, "synthetic-split-2")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 300'000'000, // 3 units
                         .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-2"), "3")},
                    },
            }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::SameDayAmbiguous);
    EXPECT_EQ(result.mActions.at(0).mEvents.size(), 2U);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionAmbiguous);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionAmbiguousSameDay);
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-06-01"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-split-1"),
                                            source(4, "synthetic-split-2")}));

    EXPECT_TRUE(result.mRequests.empty());
}

TEST(CorporateActionResolverTest, SplitsOfTwoInvestmentsOnOneDayAreTwoQuestions) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share A",
            .mIsin = "XX0000000001",
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                   .mSources = {source(2, "synthetic-buy-a")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 300'000'000,       // 3 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                   .mSources = {source(6, "synthetic-sell-a")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                   .mSources = {source(4, "synthetic-split-a")}},
                     .mType = CorporateActionType::UnresolvedSplit,
                     .mUnitsDelta = 300'000'000, // 3 units
                     .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-a"), "3")},
                },
        },
        {
            .mName = "Synthetic Share B",
            .mIsin = "XX0000000002",
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                   .mSources = {source(3, "synthetic-buy-b")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                   .mSources = {source(7, "synthetic-sell-b")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                   .mSources = {source(5, "synthetic-split-b")}},
                     .mType = CorporateActionType::UnresolvedSplit,
                     .mUnitsDelta = -900'000'000, // -9 units
                     .mUnitEvidence = sharesEvidence(source(5, "synthetic-split-b"), "-9")},
                },
        },
    };

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 2U);
    EXPECT_EQ(result.mActions.at(0).mIsin, "XX0000000001");
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    EXPECT_EQ(result.mActions.at(1).mIsin, "XX0000000002");
    EXPECT_EQ(result.mActions.at(1).mState, CorporateActionState::WaitingForRatio);

    ASSERT_EQ(result.mRequests.size(), 2U);

    const auto& first =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);
    const auto& second =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(1).mQuestion.mData);

    EXPECT_EQ(first.mIsin, "XX0000000001");
    EXPECT_EQ(first.mPositionBefore, 300'000'000); // 3 units
    EXPECT_EQ(second.mIsin, "XX0000000002");
    EXPECT_EQ(second.mPositionBefore, 1'000'000'000); // 10 units
    // A negative source value is shown as it is.
    EXPECT_EQ(second.mSources.at(0).mSourceValue, "-9");
}

TEST(CorporateActionResolverTest, ALaterSplitWaitsForTheEarlierOne) {
    // Two splits in one year, whatever their ratios: the position before the September split
    // depends on the March split, so only March is asked. September is listed and waits.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 300'000'000,       // 3 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {source(4, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-10-01"),
                                       .mSources = {source(6, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(3, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 300'000'000, // 3 units
                         .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "3")},
                        {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                       .mSources = {source(5, "synthetic-split-2")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 700'000'000, // 7 units
                         .mUnitEvidence = sharesEvidence(source(5, "synthetic-split-2"), "7")},
                    },
            }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 2U);
    EXPECT_EQ(result.mActions.at(0).mEffectiveDate, day("2024-03-01"));
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    EXPECT_EQ(result.mActions.at(1).mEffectiveDate, day("2024-09-01"));
    EXPECT_EQ(result.mActions.at(1).mState, CorporateActionState::WaitingForEarlierAction);
    EXPECT_EQ(result.mActions.at(1).mEvents,
              (std::vector<StatementEventReference>{{StatementEventKind::CorporateAction, 0, 1}}));

    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAmbiguousRatioRequired);

    const auto& waiting = result.mDiagnostics.at(1);

    EXPECT_EQ(waiting.mCode, ProcessingDiagnosticCode::CorporateActionNotApplied);
    EXPECT_EQ(waiting.mMessageKey, MessageKey::CorporateActionNotAppliedWaitingForEarlierAction);
    EXPECT_EQ(waiting.mMessageParams,
              (std::vector<MessageParam>{textParam("earlierDate", "2024-03-01")}));
    EXPECT_EQ(waiting.mContext.mTaxDate, day("2024-09-01"));
    EXPECT_EQ(waiting.mContext.mSources,
              (std::vector<SourceReference>{source(5, "synthetic-split-2")}));

    // Answering March is the way forward, so the request covers both errors.
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests.at(0).mProcessingDiagnostics, (std::vector<std::size_t>{0, 1}));

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mEffectiveDate, day("2024-03-01"));
    EXPECT_EQ(data.mPositionBefore, 300'000'000); // 3 units before March
    // The purchase of 2024-06-01 comes before the next split, so it limits the range.
    EXPECT_EQ(data.mEarliestDate, day("2024-01-11"));
    EXPECT_EQ(data.mLatestDate, day("2024-06-01"));
}

TEST(CorporateActionResolverTest, AConfirmedRatioIsKeptForTheLotStep) {
    // 3 units before the split; the user confirms 2 / 1 and the preview of 6 units.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-09-01"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 100'000'000,     // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 300'000'000, // 3 units
                 .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "3")},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-06-01"),
                                                              .mConfirmedUnits = 600'000'000}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.size(), 1U);

    const auto& action = latest.mActions.at(0);

    EXPECT_EQ(action.mState, CorporateActionState::RatioConfirmed);
    EXPECT_EQ(action.mRatio, (SplitRatio{.mNewShares = 2, .mOldShares = 1}));
    EXPECT_EQ(action.mConfirmedDate, day("2024-06-01"));
    EXPECT_EQ(action.mEvents,
              (std::vector<StatementEventReference>{{StatementEventKind::CorporateAction, 0, 0}}));

    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_EQ(latest.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionNotAppliedRatioConfirmed);
}

TEST(CorporateActionResolverTest, AnEnteredRatioIsKeptReduced) {
    // 20 new for 10 old is 2 / 1: 3 units become 6.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-09-01"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 100'000'000,     // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 300'000'000, // 3 units
                 .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "3")},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 20,
                                                              .mOldShares = 10,
                                                              .mEffectiveDate = day("2024-06-01"),
                                                              .mConfirmedUnits = 600'000'000}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::RatioConfirmed);
    EXPECT_EQ(latest.mActions.at(0).mRatio, (SplitRatio{.mNewShares = 2, .mOldShares = 1}));
    EXPECT_EQ(
        latest.mDiagnostics.at(0).mMessageParams,
        (std::vector<MessageParam>{numberParam("newShares", 2), numberParam("oldShares", 1)}));
}

TEST(CorporateActionResolverTest, ACancelledRatioQuestionIsKeptAsCancelled) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-09-01"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 100'000'000,     // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 300'000'000, // 3 units
                 .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "3")},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, CancelAnswer{}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Cancelled);
    EXPECT_FALSE(latest.mActions.at(0).mRatio.has_value());

    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_EQ(latest.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::CorporateActionAmbiguous);
    EXPECT_EQ(latest.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAmbiguousCancelled);
}

TEST(CorporateActionResolverTest, OneActionAtTwoBrokersGetsOneRatio) {
    // 2 units at Trade Republic and 1 at Interactive Brokers, one split reported by both. The
    // answer 2 / 1 for the whole 3 units is kept once for the action, not once per broker (that
    // would give 12 units).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 200'000'000,       // 2 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-02-10"),
                                       .mSources = {ibkrSource(2, "synthetic-ibkr-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                       .mSources = {source(4, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {source(3, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 200'000'000, // 2 units
                         .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "2")},
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {ibkrSource(3, "synthetic-ibkr-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                    },
            }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    // 3 units with 2 / 1 are 6 units.
    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-06-01"),
                                                              .mConfirmedUnits = 600'000'000}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.size(), 1U);

    const auto& action = latest.mActions.at(0);

    EXPECT_EQ(action.mState, CorporateActionState::RatioConfirmed);
    EXPECT_EQ(action.mRatio, (SplitRatio{.mNewShares = 2, .mOldShares = 1}));
    EXPECT_EQ(action.mEvents,
              (std::vector<StatementEventReference>{{StatementEventKind::CorporateAction, 0, 0},
                                                    {StatementEventKind::CorporateAction, 0, 1}}));

    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_EQ(latest.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mDiagnostics.at(0).mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-split-1"),
                                            ibkrSource(3, "synthetic-ibkr-split-1")}));
}

TEST(CorporateActionResolverTest, ALaterSplitStillWaitsAfterTheFirstIsConfirmed) {
    // March is confirmed. September is not asked yet: its position needs March applied first.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 300'000'000,       // 3 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-10-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(3, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 300'000'000, // 3 units
                         .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "3")},
                        {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                       .mSources = {source(4, "synthetic-split-2")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 600'000'000, // 6 units
                         .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-2"), "6")},
                    },
            }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-03-01"),
                                                              .mConfirmedUnits = 600'000'000}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.size(), 2U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::RatioConfirmed);
    EXPECT_EQ(latest.mActions.at(0).mRatio, (SplitRatio{.mNewShares = 2, .mOldShares = 1}));
    EXPECT_EQ(latest.mActions.at(1).mState, CorporateActionState::WaitingForEarlierAction);
    EXPECT_FALSE(latest.mActions.at(1).mRatio.has_value());

    // No open question is left, so the waiting error is not linked to one.
    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_EQ(latest.mDiagnostics.size(), 2U);
    EXPECT_EQ(latest.mDiagnostics.at(1).mMessageKey,
              MessageKey::CorporateActionNotAppliedWaitingForEarlierAction);
}

TEST(CorporateActionResolverTest, TwoRowsOfOneBrokerOnALaterDayAreAmbiguous) {
    // March is asked. September has two rows of one broker with different transaction IDs: one
    // action or two is unknown, whatever March's answer is, so it is not a waiting action.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 300'000'000,       // 3 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-10-01"),
                                       .mSources = {source(6, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(3, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 300'000'000, // 3 units
                         .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "3")},
                        {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                       .mSources = {source(4, "synthetic-split-2")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 600'000'000, // 6 units
                         .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-2"), "6")},
                        {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                       .mSources = {source(5, "synthetic-split-3")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 600'000'000, // 6 units
                         .mUnitEvidence = sharesEvidence(source(5, "synthetic-split-3"), "6")},
                    },
            }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 2U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    EXPECT_EQ(result.mActions.at(1).mState, CorporateActionState::SameDayAmbiguous);
    EXPECT_EQ(result.mActions.at(1).mEvents,
              (std::vector<StatementEventReference>{{StatementEventKind::CorporateAction, 0, 1},
                                                    {StatementEventKind::CorporateAction, 0, 2}}));

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& sameDay = result.mDiagnostics.at(1);

    EXPECT_EQ(sameDay.mCode, ProcessingDiagnosticCode::CorporateActionAmbiguous);
    EXPECT_EQ(sameDay.mMessageKey, MessageKey::CorporateActionAmbiguousSameDay);
    EXPECT_EQ(sameDay.mContext.mTaxDate, day("2024-09-01"));
    EXPECT_EQ(sameDay.mContext.mSources,
              (std::vector<SourceReference>{source(4, "synthetic-split-2"),
                                            source(5, "synthetic-split-3")}));

    // Answering March does not resolve it, so the request covers only March.
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests.at(0).mProcessingDiagnostics, (std::vector<std::size_t>{0}));
}

TEST(CorporateActionResolverTest, APositionThatDoesNotFitUnitsIsANumericError) {
    // 50'000'000'000 units are 5'000'000'000'000'000'000 raw units. Two lots add up to
    // 10'000'000'000'000'000'000, which is above the largest int64 (9'223'372'036'854'775'807).
    // FIFO checks the open units only at a sale, and the sale comes after the split.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-01-02"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 100'000'000,           // EUR 1
                         .mUnits = 5'000'000'000'000'000'000, // 50'000'000'000 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-01-03"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 100'000'000,           // EUR 1
                         .mUnits = 5'000'000'000'000'000'000, // 50'000'000'000 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 100'000'000, // EUR 1
                         .mUnits = 100'000'000,     // 1 unit
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {source(4, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 100'000'000, // 1 unit
                         .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-1"), "1")},
                    },
            }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::NumericFailure);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    // At the corporate-action stage an overflow means the action cannot keep the position.
    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionUnrepresentable);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionUnrepresentable);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::CorporateAction);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-06-01"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(4, "synthetic-split-1")}));

    EXPECT_TRUE(result.mRequests.empty());
}

TEST(CorporateActionResolverTest, AnUnknownClassSplitIsAsked) {
    // The user may still classify the investment as a share, so its ratio is asked.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Investment",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Unknown,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-15"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000, // 1 unit
                 .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "1")},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData)
                  .mPositionBefore,
              100'000'000); // 1 unit
}

TEST(CorporateActionResolverTest, AFundSplitIsAsked) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Fund",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Fund,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-15"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000, // 1 unit
                 .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "1")},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(CorporateActionResolverTest, ABondSplitIsNotAsked) {
    // Bond sales stay outside Doh-KDVP, so a ratio answer could not lead to a report.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Bond,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-15"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000, // 1 unit
                 .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "1")},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::NotAskedForClass);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionNotApplied);

    EXPECT_TRUE(result.mRequests.empty());
}

TEST(CorporateActionResolverTest, ASplitOn31DecemberIsAsked) {
    // 31 December is still in the 2024 ledger (ASplitAfter31DecemberIsNotAsked is the other side).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-12-31"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000, // 1 unit
                 .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-1"), "1")},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mEffectiveDate, day("2024-12-31"));
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);

    ASSERT_EQ(result.mRequests.size(), 1U);
    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mPositionBefore, 100'000'000); // 2 bought, 1 sold before the split
    // The day after the sale, up to 31 December itself.
    EXPECT_EQ(data.mEarliestDate, day("2024-03-11"));
    EXPECT_EQ(data.mLatestDate, day("2024-12-31"));
}

TEST(CorporateActionResolverTest, TheNextSplitLimitsTheEffectiveDate) {
    // No trade between the splits, so the March split may be dated up to the day before the
    // September split, not later: then the two would change order.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 300'000'000,       // 3 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-10-01"),
                                       .mSources = {source(5, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(3, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 300'000'000, // 3 units
                         .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "3")},
                        {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                       .mSources = {source(4, "synthetic-split-2")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 600'000'000, // 6 units
                         .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-2"), "6")},
                    },
            }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mEffectiveDate, day("2024-03-01"));
    EXPECT_EQ(data.mEarliestDate, day("2024-01-11"));
    EXPECT_EQ(data.mLatestDate, day("2024-08-31")); // the day before the September split
}

TEST(CorporateActionResolverTest, TheYearEndLimitsTheEffectiveDate) {
    // No trade after the split: a date after 31 December would take the split out of 2024.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000, // 1 unit
                 .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-1"), "1")},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mEarliestDate, day("2024-03-11")); // the day after the sale
    EXPECT_EQ(data.mLatestDate, day("2024-12-31"));
}

TEST(CorporateActionResolverTest, TheAnnouncementDateIsKeptBesideTheRowDate) {
    // The broker's row is dated 2024-06-01; the announcement says 2024-05-20. No trade lies
    // between, so the position is the same and the stage keeps both dates.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-09-01"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 100'000'000,     // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 300'000'000, // 3 units
                 .mUnitEvidence = sharesEvidence(source(3, "synthetic-split-1"), "3")},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-05-20"),
                                                              .mConfirmedUnits = 600'000'000}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::RatioConfirmed);
    EXPECT_EQ(latest.mActions.at(0).mEffectiveDate, day("2024-06-01")); // the broker's row
    EXPECT_EQ(latest.mActions.at(0).mConfirmedDate, day("2024-05-20")); // the announcement
}

} // namespace
