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
    RunDecisions decisions;

    return matchCapitalPools(aMerged.mStatement, prepared.mLedger.value(), decisions).mActions;
}

/// A session calculation that matches the pools and resolves the corporate actions with the
/// session's answers, so a confirmed action is applied and the next one is asked.
/// aLatest keeps the stage result of the latest calculation, so a test can check it after an
/// answer.
RunCalculation
resolvingCalculation(StatementMergeResult& aMerged, int aYear, CorporateActionResult& aLatest) {
    orderChronologically(aMerged.mStatement);

    return [&aMerged, aYear, &aLatest](RunDecisions& aDecisions) {
        const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});

        aLatest =
            matchCapitalPools(aMerged.mStatement, prepared.mLedger.value(), aDecisions).mActions;

        ApplicationResultInput input{.mReportingYear = aYear};
        auto diagnostics = aLatest.mDiagnostics;
        auto requests = aLatest.mRequests;

        appendStageOutput(input, std::move(diagnostics), std::move(requests));

        return input;
    };
}

/// Like resolvingCalculation, but aLatest keeps the FIFO pools of the latest calculation too, so
/// a test can check the lots and the FIFO diagnostics.
RunCalculation poolsCalculation(StatementMergeResult& aMerged, int aYear, CapitalPools& aLatest) {
    orderChronologically(aMerged.mStatement);

    return [&aMerged, aYear, &aLatest](RunDecisions& aDecisions) {
        const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});

        aLatest = matchCapitalPools(aMerged.mStatement, prepared.mLedger.value(), aDecisions);

        ApplicationResultInput input{.mReportingYear = aYear};
        auto diagnostics = aLatest.mActions.mDiagnostics;
        auto requests = aLatest.mActions.mRequests;

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
                                                        .mDate = day("2023-01-15"),
                                                        .mSourceValue = "1"}}));
    EXPECT_FALSE(data.mAskSameEvent);
    EXPECT_EQ(data.mPositionBefore, 100'000'000); // 1 unit bought before the split
    // The day after the purchase of 2022-01-10, up to the sale on 2024-03-10: any date between
    // has the 1 unit before the split. From 2024-03-11 the sale of 1 unit would come first and
    // leave 0 units, so no later date is in a range. The sale of 1 unit comes after the split
    // on every date of the range.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2022-01-11"),
                                                .mLatestDate = day("2024-03-10"),
                                                .mPositionBefore = 100'000'000,
                                                .mLowestChangeAfter = -100'000'000}}));
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

    EXPECT_EQ(
        data.mSources,
        (std::vector<CorporateActionSourceFacts>{
            {.mBroker = Broker::TradeRepublic, .mDate = day("2024-06-01"), .mSourceValue = "3"}}));
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
    // The gates block a crypto sale with a class error anyway, so a ratio answer could not lead
    // to a report, and the action needs no error of its own.
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

    EXPECT_TRUE(result.mActions.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(CorporateActionResolverTest, NoUnitsBeforeTheSplitIsAMissingPosition) {
    // 2 units bought and 2 sold before the split: the files show nothing to split, so a split row
    // points to missing history. No question is asked.
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
    EXPECT_EQ(result.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::CorporateActionMissingPosition);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionMissingPosition);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mTaxDate, day("2023-06-01"));

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
    EXPECT_EQ(data.mSources,
              (std::vector<CorporateActionSourceFacts>{{.mBroker = Broker::TradeRepublic,
                                                        .mTransactionId = "synthetic-split-1",
                                                        .mDate = day("2024-06-01"),
                                                        .mSourceValue = "2"},
                                                       {.mBroker = Broker::InteractiveBrokers,
                                                        .mTransactionId = "synthetic-ibkr-split-1",
                                                        .mDate = day("2024-06-01")}}));
    // Both rows are on one date, so the question does not ask whether they are one event.
    EXPECT_FALSE(data.mAskSameEvent);
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
    // Up to the purchase of 2024-06-01 the split comes after 3 units; from the next day after
    // 3 + 1 = 4. The day before the September split ends the last range.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-06-01"),
                                                .mPositionBefore = 300'000'000},
                                               {.mEarliestDate = day("2024-06-02"),
                                                .mLatestDate = day("2024-08-31"),
                                                .mPositionBefore = 400'000'000}}));
}

TEST(CorporateActionResolverTest, AConfirmedRatioIsAppliedWithAWarning) {
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

    EXPECT_EQ(action.mState, CorporateActionState::Applied);
    EXPECT_EQ(action.mRatio, (SplitRatio{.mNewShares = 2, .mOldShares = 1}));
    EXPECT_EQ(action.mConfirmedDate, day("2024-06-01"));
    EXPECT_EQ(action.mEvents,
              (std::vector<StatementEventReference>{{StatementEventKind::CorporateAction, 0, 0}}));

    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_EQ(latest.mDiagnostics.size(), 1U);

    // A warning, never an error: the ratio comes from the user, not from the broker.
    const auto& diagnostic = latest.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionApplied);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionAppliedUserRatio);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{numberParam("newShares", 2),
                                         numberParam("oldShares", 1),
                                         textParam("effectiveDate", "2024-06-01")}));
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-06-01"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-split-1")}));
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
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(latest.mActions.at(0).mRatio, (SplitRatio{.mNewShares = 2, .mOldShares = 1}));
    EXPECT_EQ(latest.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{numberParam("newShares", 2),
                                         numberParam("oldShares", 1),
                                         textParam("effectiveDate", "2024-06-01")}));
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

    EXPECT_EQ(action.mState, CorporateActionState::Applied);
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

TEST(CorporateActionResolverTest, TheNextSplitIsAskedWithThePositionAfterTheEarlierOne) {
    // March is confirmed and applied: 3 units become 6. September is asked with those 6 units.
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
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(latest.mActions.at(0).mRatio, (SplitRatio{.mNewShares = 2, .mOldShares = 1}));
    EXPECT_EQ(latest.mActions.at(1).mState, CorporateActionState::WaitingForRatio);
    EXPECT_FALSE(latest.mActions.at(1).mRatio.has_value());

    ASSERT_EQ(latest.mDiagnostics.size(), 2U);
    EXPECT_EQ(latest.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAppliedUserRatio);
    EXPECT_EQ(latest.mDiagnostics.at(1).mMessageKey,
              MessageKey::CorporateActionAmbiguousRatioRequired);
    EXPECT_EQ(latest.mDiagnostics.at(1).mContext.mTaxDate, day("2024-09-01"));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mEffectiveDate, day("2024-09-01"));
    EXPECT_EQ(data.mPositionBefore, 600'000'000); // 3 units x 2 / 1 = 6 units
    // From the day after the March split up to the sale of 1 unit on 2024-10-01: 6 units, and
    // the sale of 1 unit after the split; from the next day 6 - 1 = 5 units, up to 31 December,
    // with no trade after it.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-03-02"),
                                                .mLatestDate = day("2024-10-01"),
                                                .mPositionBefore = 600'000'000,
                                                .mLowestChangeAfter = -100'000'000},
                                               {.mEarliestDate = day("2024-10-02"),
                                                .mLatestDate = day("2024-12-31"),
                                                .mPositionBefore = 500'000'000}}));
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
    // Bond sales stay outside Doh-KDVP and the gates block them with a class error, so a ratio
    // answer could not lead to a report, and the action needs no error of its own.
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

    EXPECT_TRUE(result.mActions.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
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
    // Up to the sale 2 units, with the sale of 1 unit after the split; then 2 - 1 = 1 unit up
    // to 31 December itself.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-03-10"),
                                                .mPositionBefore = 200'000'000,
                                                .mLowestChangeAfter = -100'000'000},
                                               {.mEarliestDate = day("2024-03-11"),
                                                .mLatestDate = day("2024-12-31"),
                                                .mPositionBefore = 100'000'000}}));
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
    // The day before the September split ends the range.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-08-31"),
                                                .mPositionBefore = 300'000'000}}));
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

    // 2 units up to the sale (the sale of 1 unit then comes after the split), then 1 unit up to
    // 31 December, not later.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-03-10"),
                                                .mPositionBefore = 200'000'000,
                                                .mLowestChangeAfter = -100'000'000},
                                               {.mEarliestDate = day("2024-03-11"),
                                                .mLatestDate = day("2024-12-31"),
                                                .mPositionBefore = 100'000'000}}));
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
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(latest.mActions.at(0).mEffectiveDate, day("2024-06-01")); // the broker's row
    EXPECT_EQ(latest.mActions.at(0).mConfirmedDate, day("2024-05-20")); // the announcement
}

TEST(CorporateActionResolverTest, SplitRowsOfTwoBrokersTenDaysApartAreAskedAsOneEvent) {
    // Trade Republic books the split on 2024-06-01, Interactive Brokers on 2024-06-11: most
    // likely one split booked twice. One ratio question lists both rows and asks whether they are
    // one event (maintainer decision, 2026-10-29). 10 days is still inside the window.
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
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-06-11"),
                               .mSources = {ibkrSource(3, "synthetic-ibkr-split-1")}},
                 .mType = CorporateActionType::Split,
                 .mRatio = SplitRatio{.mNewShares = 2, .mOldShares = 1}},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    EXPECT_EQ(result.mActions.at(0).mEvents,
              (std::vector<StatementEventReference>{{StatementEventKind::CorporateAction, 0, 0},
                                                    {StatementEventKind::CorporateAction, 0, 1}}));

    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_TRUE(data.mAskSameEvent);
    EXPECT_EQ(data.mEffectiveDate, day("2024-06-01")); // the first row
    EXPECT_EQ(data.mSources,
              (std::vector<CorporateActionSourceFacts>{{.mBroker = Broker::TradeRepublic,
                                                        .mTransactionId = "synthetic-split-1",
                                                        .mDate = day("2024-06-01")},
                                                       {.mBroker = Broker::InteractiveBrokers,
                                                        .mTransactionId = "synthetic-ibkr-split-1",
                                                        .mDate = day("2024-06-11")}}));
    EXPECT_EQ(data.mPositionBefore, 300'000'000); // 3 units on 2024-06-01

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAmbiguousRatioRequired);
}

TEST(CorporateActionResolverTest, TwoBrokerRowsAnsweredAsOneSplitAreAppliedOnce) {
    // 3 units, split rows of Trade Republic on 2024-06-01 and Interactive Brokers on 2024-06-11, a
    // sale of 1 unit on 2024-09-01. 2 : 1 from 2024-06-01 makes the 3 units 6, once for both rows
    // (not 12), and the sale leaves 5.
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
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-06-11"),
                               .mSources = {ibkrSource(3, "synthetic-ibkr-split-1")}},
                 .mType = CorporateActionType::Split,
                 .mRatio = SplitRatio{.mNewShares = 2, .mOldShares = 1}},
            },
    }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-06-01"),
                                                              .mConfirmedUnits = 600'000'000}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(latest.mActions.mActions.at(0).mEvents.size(), 2U);

    EXPECT_TRUE(latest.mActions.mRequests.empty());
    // Only the warning that the user supplied the ratio, once.
    ASSERT_EQ(latest.mActions.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mActions.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAppliedUserRatio);

    const auto& pool = latest.mFifo.mIsins.at(0);

    EXPECT_EQ(pool.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(pool.mOpenLots.size(), 1U);
    EXPECT_EQ(pool.mOpenLots.at(0).mState.mRemainingUnits, 500'000'000); // 5 units
}

TEST(CorporateActionResolverTest, TwoBrokerRowsAnsweredAsTwoSplitsAreAmbiguous) {
    // 3 units, split rows of Trade Republic on 2024-06-01 and Interactive Brokers on 2024-06-11, a
    // sale of 1 unit on 2024-09-01. The user says the rows are two splits. Two splits within 10
    // days are not supported yet, so the investment blocks.
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
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-06-11"),
                               .mSources = {ibkrSource(3, "synthetic-ibkr-split-1")}},
                 .mType = CorporateActionType::Split,
                 .mRatio = SplitRatio{.mNewShares = 2, .mOldShares = 1}},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, SeparateEventsAnswer{}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::DifferentDatesAmbiguous);
    EXPECT_FALSE(latest.mActions.at(0).mRatio.has_value());

    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_EQ(latest.mDiagnostics.size(), 1U);

    const auto& diagnostic = latest.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionAmbiguous);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionAmbiguousDifferentDates);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{numberParam("days", 10)}));
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-06-01"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-split-1"),
                                            ibkrSource(3, "synthetic-ibkr-split-1")}));
}

TEST(CorporateActionResolverTest, TwoSplitsIsNoAnswerToAQuestionWithRowsOnOneDate) {
    // One row, so there is nothing to tell apart.
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
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, SeparateEventsAnswer{}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::AnswerDoesNotFit);
    EXPECT_EQ(latest.mRequests.size(), 1U);
}

TEST(CorporateActionResolverTest, ThreeNearbySplitRowsOfTwoBrokersAreAmbiguous) {
    // Trade Republic on 2024-06-01 and 2024-06-09, Interactive Brokers on 2024-06-05 between
    // them: which rows are one split is unknown, so all three block and nothing is asked.
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
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-06-05"),
                               .mSources = {ibkrSource(3, "synthetic-ibkr-split-1")}},
                 .mType = CorporateActionType::Split},
                {.mMetadata = {.mTaxDate = day("2024-06-09"),
                               .mSources = {source(4, "synthetic-split-2")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    EXPECT_TRUE(result.mRequests.empty());
    ASSERT_EQ(result.mActions.size(), 3U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::DifferentDatesAmbiguous);
    EXPECT_EQ(result.mActions.at(1).mState, CorporateActionState::DifferentDatesAmbiguous);
    EXPECT_EQ(result.mActions.at(2).mState, CorporateActionState::DifferentDatesAmbiguous);

    ASSERT_EQ(result.mDiagnostics.size(), 3U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAmbiguousDifferentDates);
    EXPECT_EQ(result.mDiagnostics.at(1).mMessageKey,
              MessageKey::CorporateActionAmbiguousDifferentDates);
    EXPECT_EQ(result.mDiagnostics.at(2).mMessageKey,
              MessageKey::CorporateActionAmbiguousDifferentDates);
}

TEST(CorporateActionResolverTest, SplitRowsOfTwoBrokersElevenDaysApartAreTwoActions) {
    // 2024-06-01 and 2024-06-12 are 11 days apart, outside the window: two actions, the first
    // is asked and the second waits.
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
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-06-12"),
                               .mSources = {ibkrSource(3, "synthetic-ibkr-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 2U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    EXPECT_EQ(result.mActions.at(1).mState, CorporateActionState::WaitingForEarlierAction);
    ASSERT_EQ(result.mRequests.size(), 1U);
}

TEST(CorporateActionResolverTest, SplitRowsOfOneBrokerOnNearbyDatesAreTwoActions) {
    // Two split rows of Trade Republic five days apart: one broker does not book one split on two
    // dates, so they are two actions; the first is asked.
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
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-06-06"),
                               .mSources = {source(4, "synthetic-split-2")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 2U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    EXPECT_EQ(result.mActions.at(1).mState, CorporateActionState::WaitingForEarlierAction);
}

TEST(CorporateActionResolverTest, AMergerIsUnsupportedAndNotAsked) {
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
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {ibkrSource(3, "synthetic-ibkr-merger-1")}},
                 .mType = CorporateActionType::Merger},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    EXPECT_TRUE(result.mRequests.empty());
    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::Unsupported);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::UnsupportedCorporateAction);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::UnsupportedCorporateActionMerger);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mSources,
              (std::vector<SourceReference>{ibkrSource(3, "synthetic-ibkr-merger-1")}));
}

TEST(CorporateActionResolverTest, AMergerAfterASplitIsUnsupportedNotWaiting) {
    // The March split is asked. The September merger is not a split, so it does not wait for
    // March: it is unsupported whatever the answer.
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
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-09-01"),
                               .mSources = {ibkrSource(4, "synthetic-ibkr-merger-1")}},
                 .mType = CorporateActionType::Merger},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 2U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    EXPECT_EQ(result.mActions.at(1).mState, CorporateActionState::Unsupported);
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(1).mMessageKey, MessageKey::UnsupportedCorporateActionMerger);
}

TEST(CorporateActionResolverTest, AMergerOfAnotherBrokerNearASplitIsNotTheSameSplit) {
    // A merger row of Interactive Brokers 5 days after the Trade Republic split row: a merger is
    // never one split booked twice, so the split is still asked; the merger blocks on its own.
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
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-06-06"),
                               .mSources = {ibkrSource(3, "synthetic-ibkr-merger-1")}},
                 .mType = CorporateActionType::Merger},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 2U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    EXPECT_EQ(result.mActions.at(1).mState, CorporateActionState::Unsupported);
    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(CorporateActionResolverTest, DatesOutsideTheRangesOrWithoutUnitsAreRejected) {
    // 1 unit bought 2024-01-10 and sold 2024-03-01, 1 bought 2024-05-01, split row 2024-06-01,
    // 1 sold 2024-07-01, another split 2024-10-01. With units before the split: 2024-01-11 to
    // 2024-03-01 (1 unit) and 2024-05-02 to 2024-07-01 (1 unit). From 2024-03-02 to 2024-05-01
    // no unit is held, and from 2024-07-02 none either.
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
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(3, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'200'000'000, // EUR 12
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-05-01"),
                                       .mSources = {source(4, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-07-01"),
                                       .mSources = {source(6, "synthetic-sell-2")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 600'000'000, // EUR 6
                         .mUnits = 200'000'000,     // 2 units, after the split
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {source(5, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                        {.mMetadata = {.mTaxDate = day("2024-10-01"),
                                       .mSources = {source(7, "synthetic-split-2")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                    },
            }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(latest.mRequests.at(0).mQuestion.mData);

    // The sale of 2 units on 2024-07-01 leaves 1 - 2 = -1 old units, so no later date is offered.
    // After a date of the first range come -1, +1 and -2 units (lowest -1 + 1 - 2 = -2), after
    // one of the second range the sale of 2 units.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-03-01"),
                                                .mPositionBefore = 100'000'000,
                                                .mLowestChangeAfter = -200'000'000},
                                               {.mEarliestDate = day("2024-05-02"),
                                                .mLatestDate = day("2024-07-01"),
                                                .mPositionBefore = 100'000'000,
                                                .mLowestChangeAfter = -200'000'000}}));

    // 1 unit with 2 / 1 is 2 units on every allowed date.
    const auto answerOn = [&session](std::string_view aDate) {
        return session.answer({{"request-0001",
                                1,
                                SplitRatioAnswer{.mNewShares = 2,
                                                 .mOldShares = 1,
                                                 .mEffectiveDate = day(aDate),
                                                 .mConfirmedUnits = 200'000'000}}});
    };

    const auto beforeTheFirstPurchase = answerOn("2024-01-10");
    const auto withoutUnits = answerOn("2024-04-01");
    const auto onTheNextSplit = answerOn("2024-10-01");
    const auto afterTheYear = answerOn("2025-01-01");

    ASSERT_EQ(beforeTheFirstPurchase.size(), 1U);
    EXPECT_EQ(beforeTheFirstPurchase.at(0).mReason, AnswerRejection::EffectiveDateOutOfRange);
    EXPECT_EQ(beforeTheFirstPurchase.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0001"),
                                         textParam("earliestDate", "2024-01-11"),
                                         textParam("latestDate", "2024-07-01")}));

    ASSERT_EQ(withoutUnits.size(), 1U);
    EXPECT_EQ(withoutUnits.at(0).mReason, AnswerRejection::EffectiveDateWithoutUnits);
    EXPECT_EQ(withoutUnits.at(0).mMessageKey, MessageKey::AnswerRejectedEffectiveDateWithoutUnits);
    EXPECT_EQ(withoutUnits.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0001")}));

    ASSERT_EQ(onTheNextSplit.size(), 1U);
    EXPECT_EQ(onTheNextSplit.at(0).mReason, AnswerRejection::EffectiveDateOutOfRange);
    ASSERT_EQ(afterTheYear.size(), 1U);
    EXPECT_EQ(afterTheYear.at(0).mReason, AnswerRejection::EffectiveDateOutOfRange);

    // An earlier date with units is accepted: the split comes before the March sale, and its 2
    // units cover the later sales (2 - 2 = 0).
    EXPECT_TRUE(answerOn("2024-02-01").empty());
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(latest.mActions.at(0).mConfirmedDate, day("2024-02-01"));
}

TEST(CorporateActionResolverTest, TradesOfOneDayCountTogetherInTheDateRanges) {
    // 3 units bought 2024-01-10; on 2024-02-01 1 bought and 1 sold, together 0, so the units
    // before the split stay 3 and the two ranges around that day join into one. The sale of
    // 2024-12-31 counts only from the next day, which is after 31 December, so the range ends
    // there with 3 units.
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
                        {.mMetadata = {.mTaxDate = day("2024-02-01"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-02-01"),
                                       .mSources = {source(4, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'200'000'000, // EUR 12
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-12-31"),
                                       .mSources = {source(6, "synthetic-sell-2")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'200'000'000, // EUR 12
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-06-01"),
                                       .mSources = {source(5, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                    },
            }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mPositionBefore, 300'000'000); // 3 + 1 - 1 units
    // The sale of 1 unit on 2024-12-31 comes after the split on every date of the range.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-12-31"),
                                                .mPositionBefore = 300'000'000,
                                                .mLowestChangeAfter = -100'000'000}}));
}

TEST(CorporateActionResolverTest, TheNextSplitCannotBeDatedOnOrBeforeTheEarlierOne) {
    // March is applied with 2024-03-01. The September question starts the day after, 2024-03-02.
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
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-09-01"),
                               .mSources = {source(4, "synthetic-split-2")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    EXPECT_TRUE(session
                    .answer({{"request-0001",
                              1,
                              SplitRatioAnswer{.mNewShares = 2,
                                               .mOldShares = 1,
                                               .mEffectiveDate = day("2024-03-01"),
                                               .mConfirmedUnits = 600'000'000}}})
                    .empty());

    // 6 units with 2 / 1 are 12 units; on 2024-03-01 itself the order of the two is unknown.
    const auto sameDay = session.answer({{"request-0001",
                                          2,
                                          SplitRatioAnswer{.mNewShares = 2,
                                                           .mOldShares = 1,
                                                           .mEffectiveDate = day("2024-03-01"),
                                                           .mConfirmedUnits = 1'200'000'000}}});

    ASSERT_EQ(sameDay.size(), 1U);
    EXPECT_EQ(sameDay.at(0).mReason, AnswerRejection::EffectiveDateOutOfRange);
    EXPECT_EQ(sameDay.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0001"),
                                         textParam("earliestDate", "2024-03-02"),
                                         textParam("latestDate", "2024-12-31")}));
}

TEST(CorporateActionResolverTest, ASplitThatCannotBeAppliedEndsTheQuestions) {
    // Lots of 0.00000001 and 1 unit, 1 / 10 confirmed: the total 1.00000001 previews as
    // 0.10000000 units, but the small lot would become 0, so the FIFO stage stops the pool with
    // corporate_action_unrepresentable. The stage lists neither action and asks nothing more.
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
                         .mUnits = 1,                 // 0.00000001 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-01-11"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 100'000'000,       // 1 unit
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-10-01"),
                                       .mSources = {source(6, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 1'000'000,         // 0.01 units
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(4, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                        {.mMetadata = {.mTaxDate = day("2024-09-01"),
                                       .mSources = {source(5, "synthetic-split-2")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                    },
            }};
    orderChronologically(merged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({2024, merged});
    CapitalPools pools;
    DecisionSession session;
    session.start([&merged, &prepared, &pools](RunDecisions& aDecisions) {
        pools = matchCapitalPools(merged.mStatement, prepared.mLedger.value(), aDecisions);

        ApplicationResultInput input{.mReportingYear = 2024};
        auto diagnostics = pools.mActions.mDiagnostics;
        auto requests = pools.mActions.mRequests;

        appendStageOutput(input, std::move(diagnostics), std::move(requests));

        return input;
    });

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 1,
                                                              .mOldShares = 10,
                                                              .mEffectiveDate = day("2024-03-01"),
                                                              .mConfirmedUnits = 10'000'000}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(pools.mActions.mActions.empty());
    EXPECT_TRUE(pools.mActions.mDiagnostics.empty());
    EXPECT_TRUE(pools.mActions.mRequests.empty());

    const auto& isin = pools.mFifo.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::NumericFailure);
    EXPECT_EQ(isin.mStoppedAt,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 0}));
    ASSERT_EQ(pools.mFifo.mDiagnostics.size(), 1U);
    EXPECT_EQ(pools.mFifo.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::CorporateActionUnrepresentable);
}

TEST(CorporateActionResolverTest, ADateThatLeavesTooFewUnitsForALaterSaleIsRejected) {
    // 10 units bought 2024-01-10, 5 sold 2024-05-01, split row 2024-06-01. On 2024-03-01 the
    // action comes before the sale of 5 units: 1 / 10 would leave 10 x 1 / 10 = 1 unit, and the
    // sale needs 5, so the answer is rejected. 1 / 2 leaves 5 units, which is enough.
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
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-05-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(latest.mRequests.at(0).mQuestion.mData);

    // Up to the sale 10 units, with the sale of 5 after the action; then 5 units, nothing after.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-05-01"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -500'000'000},
                                               {.mEarliestDate = day("2024-05-02"),
                                                .mLatestDate = day("2024-12-31"),
                                                .mPositionBefore = 500'000'000}}));

    const auto tooFew = session.answer({{"request-0001",
                                         1,
                                         SplitRatioAnswer{.mNewShares = 1,
                                                          .mOldShares = 10,
                                                          .mEffectiveDate = day("2024-03-01"),
                                                          .mConfirmedUnits = 100'000'000}}});

    // 1 unit after the action, 1 - 5 = -4 units after the sale.
    ASSERT_EQ(tooFew.size(), 1U);
    EXPECT_EQ(tooFew.at(0).mReason, AnswerRejection::LaterSaleTooLarge);
    EXPECT_EQ(tooFew.at(0).mMessageKey, MessageKey::AnswerRejectedLaterSaleTooLarge);
    EXPECT_EQ(tooFew.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0001")}));
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::WaitingForRatio);

    const auto enough = session.answer({{"request-0001",
                                         1,
                                         SplitRatioAnswer{.mNewShares = 1,
                                                          .mOldShares = 2,
                                                          .mEffectiveDate = day("2024-03-01"),
                                                          .mConfirmedUnits = 500'000'000}}});

    // 5 units after the action, 5 - 5 = 0 after the sale.
    EXPECT_TRUE(enough.empty());
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(latest.mActions.at(0).mConfirmedDate, day("2024-03-01"));
}

TEST(CorporateActionResolverTest, ASaleWithTooFewUnitsBeforeASplitRowAsksForADateBeforeTheSale) {
    // 10 units bought 2024-01-10, 20 sold 2024-06-01, split row 2024-06-03. The split may have
    // taken effect before the sale and been booked two days later, so its ratio is asked, with
    // dates up to the sale only. The row date has no units before the action (10 - 20).
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
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 2'000'000'000,   // 20 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitEvidence = sharesEvidence(source(4, "synthetic-split-1"), "10")},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::WaitingForRatio);

    ASSERT_EQ(latest.mDiagnostics.size(), 1U);

    const auto& diagnostic = latest.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionAmbiguous);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionAmbiguousRatioRequiredBeforeSale);
    EXPECT_EQ(
        diagnostic.mMessageParams,
        (std::vector<MessageParam>{textParam("saleDate", "2024-06-01"), numberParam("days", 10)}));
    // The date and source of the split row, as for every ratio question.
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-06-03"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(4, "synthetic-split-1")}));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mEffectiveDate, day("2024-06-03"));
    EXPECT_FALSE(data.mPositionBefore.has_value());
    EXPECT_EQ(data.mEarlierSaleDate, day("2024-06-01"));
    // 10 units from the day after the purchase up to the sale, which comes after the action.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-06-01"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -2'000'000'000}}));

    // The row date is after the sale, so it is in no range.
    const auto onTheRowDate =
        session.answer({{"request-0001",
                         1,
                         SplitRatioAnswer{.mNewShares = 2,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-06-03"),
                                          .mConfirmedUnits = 2'000'000'000}}});
    // 3 / 2 gives 15 units, 5 fewer than the sale needs.
    const auto tooFew = session.answer({{"request-0001",
                                         1,
                                         SplitRatioAnswer{.mNewShares = 3,
                                                          .mOldShares = 2,
                                                          .mEffectiveDate = day("2024-05-30"),
                                                          .mConfirmedUnits = 1'500'000'000}}});

    ASSERT_EQ(onTheRowDate.size(), 1U);
    EXPECT_EQ(onTheRowDate.at(0).mReason, AnswerRejection::EffectiveDateOutOfRange);
    ASSERT_EQ(tooFew.size(), 1U);
    EXPECT_EQ(tooFew.at(0).mReason, AnswerRejection::LaterSaleTooLarge);

    // 2 / 1 on 2024-05-30 gives 20 units, and the sale takes all of them.
    const auto accepted = session.answer({{"request-0001",
                                           1,
                                           SplitRatioAnswer{.mNewShares = 2,
                                                            .mOldShares = 1,
                                                            .mEffectiveDate = day("2024-05-30"),
                                                            .mConfirmedUnits = 2'000'000'000}}});

    EXPECT_TRUE(accepted.empty());
    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_EQ(latest.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Applied);
    ASSERT_EQ(latest.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAppliedUserRatio);
}

TEST(CorporateActionResolverTest,
     NoUnitsAtTheStartOfTheDayOfASaleWithTooFewUnitsIsAMissingPosition) {
    // 1 unit bought and 2 sold on the same day 2024-06-01, split row 2024-06-03. At the start of
    // that day no unit is held, and no earlier date has one, so there is no date to ask for.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 200'000'000,     // 2 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::NoPosition);
    EXPECT_TRUE(result.mRequests.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::CorporateActionMissingPosition);
}

TEST(CorporateActionResolverTest, APurchaseAfterASaleWithTooFewUnitsStillEndsTheDatesAtTheSale) {
    // 10 units bought 2024-01-10; on 2024-06-01 20 sold, then 20 bought back; split row
    // 2024-06-03. The day adds up to 0, but the sale alone needs 20 of 10 units, so no date after
    // the sale is offered: the dates end on 2024-06-01, as without the purchase.
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
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 2'000'000'000,   // 20 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(4, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 2'000'000'000,   // 20 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03"),
                               .mSources = {source(5, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_FALSE(data.mPositionBefore.has_value());
    EXPECT_EQ(data.mEarlierSaleDate, day("2024-06-01"));
    // The lowest total after the action is the sale alone: -20 units, before the purchase.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-06-01"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -2'000'000'000}}));

    // 2 / 1 on 2024-05-30 gives 20 units; the sale takes all of them, and the purchase adds 20.
    const auto accepted = session.answer({{"request-0001",
                                           1,
                                           SplitRatioAnswer{.mNewShares = 2,
                                                            .mOldShares = 1,
                                                            .mEffectiveDate = day("2024-05-30"),
                                                            .mConfirmedUnits = 2'000'000'000}}});

    EXPECT_TRUE(accepted.empty());
    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_EQ(latest.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Applied);
}

TEST(CorporateActionResolverTest, ADayThatSellsMoreThanWasHeldEndsTheDatesAlsoWhenItBuysBack) {
    // 10 units bought 2024-01-10, split row 2024-03-01; on 2024-05-01 15 sold, then 15 bought
    // back. In new units after a 2 / 1 split the sale fits (20 units). A split dated after
    // 2024-05-01 would put the sale of 15 before it, with only 10 units, so the dates end there,
    // although the day adds up to 0.
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
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-05-01"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 1'500'000'000,   // 15 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-05-01"),
                               .mSources = {source(5, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 1'500'000'000,   // 15 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mPositionBefore, 1'000'000'000); // 10 units on the row date
    // 10 units up to the sale; the lowest total after the action is the sale alone, -15 units.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-05-01"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -1'500'000'000}}));

    const auto afterTheSale =
        session.answer({{"request-0001",
                         1,
                         SplitRatioAnswer{.mNewShares = 2,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-06-01"),
                                          .mConfirmedUnits = 2'000'000'000}}});

    ASSERT_EQ(afterTheSale.size(), 1U);
    EXPECT_EQ(afterTheSale.at(0).mReason, AnswerRejection::EffectiveDateOutOfRange);
    EXPECT_EQ(afterTheSale.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0001"),
                                         textParam("earliestDate", "2024-01-11"),
                                         textParam("latestDate", "2024-05-01")}));

    // 2 / 1 on the row date gives 20 units; the sale leaves 5, and the purchase adds 15.
    const auto onTheRowDate =
        session.answer({{"request-0001",
                         1,
                         SplitRatioAnswer{.mNewShares = 2,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-03-01"),
                                          .mConfirmedUnits = 2'000'000'000}}});

    EXPECT_TRUE(onTheRowDate.empty());
    ASSERT_EQ(latest.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Applied);
}

TEST(CorporateActionResolverTest, ASaleBoughtBackOnItsDayCountsOnlyForTheDatesUpToIt) {
    // 10 units bought 2024-01-10; on 2024-05-01 10 sold, then 10 bought back; split row
    // 2024-09-01. The units before the action are 10 on every date, but the sale of 10 comes
    // after the action only on the dates up to 2024-05-01. So a 1 / 10 reverse split there leaves
    // 1 unit for the sale of 10, and after 2024-05-01 no sale follows it.
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
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-05-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-05-01"),
                               .mSources = {source(4, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-09-01"),
                               .mSources = {source(5, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mPositionBefore, 1'000'000'000); // 10 - 10 + 10 units on the row date
    // Two ranges with 10 units: up to the sale the lowest total after the action is the sale
    // alone, -10 units; after it no trade follows.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-05-01"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -1'000'000'000},
                                               {.mEarliestDate = day("2024-05-02"),
                                                .mLatestDate = day("2024-12-31"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = 0}}));

    // 1 / 10 from 2024-03-01 gives 1 unit, and the sale of 10 follows it: 1 - 10 < 0.
    const auto beforeTheSale =
        session.answer({{"request-0001",
                         1,
                         SplitRatioAnswer{.mNewShares = 1,
                                          .mOldShares = 10,
                                          .mEffectiveDate = day("2024-03-01"),
                                          .mConfirmedUnits = 100'000'000}}});

    ASSERT_EQ(beforeTheSale.size(), 1U);
    EXPECT_EQ(beforeTheSale.at(0).mReason, AnswerRejection::LaterSaleTooLarge);

    // 1 / 10 on the row date gives 1 unit, and no sale follows it.
    const auto onTheRowDate = session.answer({{"request-0001",
                                               1,
                                               SplitRatioAnswer{.mNewShares = 1,
                                                                .mOldShares = 10,
                                                                .mEffectiveDate = day("2024-09-01"),
                                                                .mConfirmedUnits = 100'000'000}}});

    EXPECT_TRUE(onTheRowDate.empty());
    ASSERT_EQ(latest.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Applied);
}

TEST(CorporateActionResolverTest, ASaleUpToTenDaysBeforeTheNextSplitRowIsLeftToThatSplit) {
    // 10 units bought 2024-01-10, split rows 2024-03-01 and 2024-06-05, 40 sold 2024-06-03. The
    // second split may have taken effect before the sale (2 : 1 from 2024-06-01: 10 -> 20 -> 40)
    // and been booked two days later, so the first split's check leaves the sale to it.
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
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-03"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 300'000'000, // EUR 3
                 .mUnits = 4'000'000'000,   // 40 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-06-05"),
                               .mSources = {source(5, "synthetic-split-2")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& first =
        std::get<CorporateActionRatioQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(first.mEffectiveDate, day("2024-03-01"));
    // 10 units up to the sale, which needs more units than were held before any split. The sale
    // on 2024-06-03 is 2 days before the next row, so up to 2024-06-02 the lowest change after
    // the action is 0. On 2024-06-03 itself the sale is after this split and before the next
    // one, so it counts: -40.
    EXPECT_EQ(first.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-06-02"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = 0},
                                               {.mEarliestDate = day("2024-06-03"),
                                                .mLatestDate = day("2024-06-03"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -4'000'000'000}}));

    // 2 / 1 from 2024-03-01 gives 20 units.
    const auto firstAnswer = session.answer({{"request-0001",
                                              1,
                                              SplitRatioAnswer{.mNewShares = 2,
                                                               .mOldShares = 1,
                                                               .mEffectiveDate = day("2024-03-01"),
                                                               .mConfirmedUnits = 2'000'000'000}}});

    EXPECT_TRUE(firstAnswer.empty());
    ASSERT_EQ(latest.mActions.size(), 2U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(latest.mActions.at(1).mState, CorporateActionState::WaitingForRatio);
    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& second =
        std::get<CorporateActionRatioQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(second.mEffectiveDate, day("2024-06-05"));
    EXPECT_FALSE(second.mPositionBefore.has_value());
    EXPECT_EQ(second.mEarlierSaleDate, day("2024-06-03"));
    // From the day after the first split up to the sale: 10 x 2 / 1 = 20 units, and the sale of
    // 40 after the action.
    EXPECT_EQ(second.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-03-02"),
                                                .mLatestDate = day("2024-06-03"),
                                                .mPositionBefore = 2'000'000'000,
                                                .mLowestChangeAfter = -4'000'000'000}}));

    // 2 / 1 from 2024-06-01 gives 40 units, and the sale takes all of them: 40 - 40 = 0.
    const auto secondAnswer =
        session.answer({{"request-0001",
                         2,
                         SplitRatioAnswer{.mNewShares = 2,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-06-01"),
                                          .mConfirmedUnits = 4'000'000'000}}});

    EXPECT_TRUE(secondAnswer.empty());
    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_EQ(latest.mActions.size(), 2U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(latest.mActions.at(1).mState, CorporateActionState::Applied);
}

TEST(CorporateActionResolverTest, ASaleOnTheFirstSplitsOwnDateCountsForTheFirstSplit) {
    // 10 units bought 2024-01-10, split rows 2024-03-01 and 2024-06-05, 40 sold 2024-06-03. A
    // first split dated 2024-06-03 comes before the sale of that day, and the second split comes
    // after the first, so it can never be before the sale: the first split must cover it.
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
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-03"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 300'000'000, // EUR 3
                 .mUnits = 4'000'000'000,   // 40 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-06-05"),
                               .mSources = {source(5, "synthetic-split-2")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    // 2 / 1 from 2024-06-03 gives 20 units, and the sale of 40 that day needs 20 more.
    const auto rejected = session.answer({{"request-0001",
                                           1,
                                           SplitRatioAnswer{.mNewShares = 2,
                                                            .mOldShares = 1,
                                                            .mEffectiveDate = day("2024-06-03"),
                                                            .mConfirmedUnits = 2'000'000'000}}});

    ASSERT_EQ(rejected.size(), 1U);
    EXPECT_EQ(rejected.at(0).mReason, AnswerRejection::LaterSaleTooLarge);
    EXPECT_EQ(rejected.at(0).mMessageKey, MessageKey::AnswerRejectedLaterSaleTooLarge);
    EXPECT_EQ(rejected.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0001")}));
    ASSERT_EQ(latest.mActions.size(), 2U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    EXPECT_EQ(latest.mActions.at(1).mState, CorporateActionState::WaitingForEarlierAction);

    // 2 / 1 from 2024-06-02, the day before the sale, leaves the sale to the second split.
    const auto accepted = session.answer({{"request-0001",
                                           1,
                                           SplitRatioAnswer{.mNewShares = 2,
                                                            .mOldShares = 1,
                                                            .mEffectiveDate = day("2024-06-02"),
                                                            .mConfirmedUnits = 2'000'000'000}}});

    EXPECT_TRUE(accepted.empty());
    ASSERT_EQ(latest.mActions.size(), 2U);
    EXPECT_EQ(latest.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(latest.mActions.at(1).mState, CorporateActionState::WaitingForRatio);
    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& second =
        std::get<CorporateActionRatioQuestion>(latest.mRequests.at(0).mQuestion.mData);

    // The second split must come on 2024-06-03 before the sale: 10 x 2 / 1 = 20 units.
    EXPECT_EQ(second.mEarlierSaleDate, day("2024-06-03"));
    EXPECT_EQ(second.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-06-03"),
                                                .mLatestDate = day("2024-06-03"),
                                                .mPositionBefore = 2'000'000'000,
                                                .mLowestChangeAfter = -4'000'000'000}}));
}

TEST(CorporateActionResolverTest, ASaleElevenDaysBeforeTheNextSplitRowCountsForTheFirstSplit) {
    // 10 units bought 2024-01-10, split rows 2024-03-01 and 2024-06-05, 40 sold 2024-05-25. The
    // sale is 11 days before the next row, so the matcher never leaves it to that split, and the
    // first split must cover it.
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
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-05-25"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 300'000'000, // EUR 3
                 .mUnits = 4'000'000'000,   // 40 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-06-05"),
                               .mSources = {source(5, "synthetic-split-2")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CorporateActionResult latest;
    DecisionSession session;
    session.start(resolvingCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(latest.mRequests.at(0).mQuestion.mData);

    // 10 units up to the sale, and the sale of 40 after the action.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-05-25"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -4'000'000'000}}));

    // 2 / 1 gives 20 units, 20 fewer than the sale needs.
    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-03-01"),
                                                              .mConfirmedUnits = 2'000'000'000}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::LaterSaleTooLarge);
}

TEST(CorporateActionResolverTest, AJanuarySplitRowInADecemberLossWindowIsAsked) {
    // 10 bought, 4 sold at a loss on 2024-12-10 (window up to 2025-01-09), split row on
    // 2025-01-05, 4 bought on 2025-01-08. The loss rule needs the ratio to compare the January
    // purchase with the sale, so it is asked, although the row is after 31 December.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-08"),
                               .mSources = {source(5, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 400'000'000, // EUR 4
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-05"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mEffectiveDate, day("2025-01-05"));
    EXPECT_EQ(data.mPositionBefore, 600'000'000); // 10 - 4 units
    EXPECT_FALSE(data.mAskSameEvent);
    // Any date up to 30 January, the end of the loss-analysis view, with the units before it:
    // 10 up to the sale, 6 up to the January purchase, then 10 again. The sale of 4 comes after
    // a date of the first range.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-03-02"),
                                                .mLatestDate = day("2024-12-10"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -400'000'000},
                                               {.mEarliestDate = day("2024-12-11"),
                                                .mLatestDate = day("2025-01-08"),
                                                .mPositionBefore = 600'000'000},
                                               {.mEarliestDate = day("2025-01-09"),
                                                .mLatestDate = day("2025-01-30"),
                                                .mPositionBefore = 1'000'000'000}}));
}

TEST(CorporateActionResolverTest, AJanuarySplitRowAfterEveryLossWindowIsNotAsked) {
    // The loss sale of 2024-12-10 has its window up to 2025-01-09; the row of 2025-01-15 is after
    // it, so the loss rule does not need it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-08"),
                               .mSources = {source(5, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 400'000'000, // EUR 4
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-15"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    EXPECT_TRUE(result.mActions.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(CorporateActionResolverTest, AJanuarySplitRowOnTheLastDayOfTheLossWindowIsAsked) {
    // 10 bought, 4 sold at a loss on 2024-12-10 (window up to 2025-01-09), split row and 4 bought
    // on 2025-01-09, the last day of the window: the row is asked. One day later it is not (next
    // test).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-09"),
                               .mSources = {source(5, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 400'000'000, // EUR 4
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-09"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mEffectiveDate, day("2025-01-09"));
    EXPECT_EQ(data.mPositionBefore, 600'000'000); // 10 - 4 units
    EXPECT_FALSE(data.mAskSameEvent);
    // 10 units up to the sale, 6 after it, and 10 again from the day after the January purchase
    // (the action comes before the trades of its own day) up to 30 January.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-03-02"),
                                                .mLatestDate = day("2024-12-10"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -400'000'000},
                                               {.mEarliestDate = day("2024-12-11"),
                                                .mLatestDate = day("2025-01-09"),
                                                .mPositionBefore = 600'000'000},
                                               {.mEarliestDate = day("2025-01-10"),
                                                .mLatestDate = day("2025-01-30"),
                                                .mPositionBefore = 1'000'000'000}}));
}

TEST(CorporateActionResolverTest, AJanuarySplitRowOneDayAfterTheLossWindowIsNotAsked) {
    // As above with the split row on 2025-01-10, one day after the window of the 2024-12-10 loss
    // (up to 2025-01-09), and the purchase on 2025-01-08: the loss rule does not need the row.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-08"),
                               .mSources = {source(5, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 400'000'000, // EUR 4
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-10"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    EXPECT_TRUE(result.mActions.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(CorporateActionResolverTest, AJanuaryDateKeepsTheLotsAtTheYearEnd) {
    // 10 bought, 4 sold at a loss on 2024-12-10, split row on 2025-01-05, 4 bought on 2025-01-08;
    // 2 : 1 from 2025-01-05 (6 units before it, 12 after). The split is used only by the loss rule:
    // the lots at 31 December keep 6 units.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-08"),
                               .mSources = {source(5, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 400'000'000, // EUR 4
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-05"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2025-01-05"),
                                                              .mConfirmedUnits = 1'200'000'000}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.mActions.at(0).mState, CorporateActionState::Applied);
    ASSERT_EQ(latest.mActions.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mActions.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAppliedUserRatio);

    const auto& pool = latest.mFifo.mIsins.at(0);

    EXPECT_TRUE(pool.mAppliedActions.empty());
    ASSERT_EQ(pool.mLaterActions.size(), 1U);
    EXPECT_EQ(pool.mLaterActions.at(0).mEffectiveDate, day("2025-01-05"));
    ASSERT_EQ(pool.mOpenLots.size(), 1U);
    EXPECT_EQ(pool.mOpenLots.at(0).mState.mRemainingUnits, 600'000'000); // 6 units
    EXPECT_FALSE(pool.mOpenLots.at(0).mChangedByAction);
}

TEST(CorporateActionResolverTest, ADecemberDateForAJanuaryRowChangesTheLots) {
    // 10 bought, 4 sold at a loss on 2024-12-10, split row on 2025-01-05, 4 bought on 2025-01-08;
    // 2 : 1 from 2024-12-20 (6 units before it): the broker booked it late, so the lots at
    // 31 December are 12 units.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-08"),
                               .mSources = {source(5, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 400'000'000, // EUR 4
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-05"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-12-20"),
                                                              .mConfirmedUnits = 1'200'000'000}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(latest.mActions.mActions.at(0).mConfirmedDate, day("2024-12-20"));

    const auto& pool = latest.mFifo.mIsins.at(0);

    ASSERT_EQ(pool.mAppliedActions.size(), 1U);
    EXPECT_TRUE(pool.mLaterActions.empty());
    ASSERT_EQ(pool.mOpenLots.size(), 1U);
    EXPECT_EQ(pool.mOpenLots.at(0).mState.mRemainingUnits, 1'200'000'000); // 12 units
    EXPECT_TRUE(pool.mOpenLots.at(0).mChangedByAction);
}

TEST(CorporateActionResolverTest, ADecemberSaleWithTooFewUnitsAsksTheJanuarySplit) {
    // 10 bought, 15 sold on 2024-12-28, split row on 2025-01-03, 6 days later: the split may
    // have taken effect before the sale, so it is asked like a row in the same year.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-28"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 1'500'000'000,     // 15 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-03"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    EXPECT_EQ(latest.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::CorporateAction);
    EXPECT_TRUE(latest.mFifo.mDiagnostics.empty()); // no incomplete_history
    ASSERT_EQ(latest.mActions.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(latest.mActions.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mEffectiveDate, day("2025-01-03"));
    EXPECT_FALSE(data.mPositionBefore.has_value());
    EXPECT_EQ(data.mEarlierSaleDate, day("2024-12-28"));
    // The 10 units up to the sale; the sale of 15 comes after the action.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-06-04"),
                                                .mLatestDate = day("2024-12-28"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -1'500'000'000}}));
    ASSERT_EQ(latest.mActions.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mActions.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAmbiguousRatioRequiredBeforeSale);

    // 2 : 1 from 2024-12-20 makes 20 units; the sale takes 15 and leaves 5.
    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-12-20"),
                                                              .mConfirmedUnits = 2'000'000'000}}});

    EXPECT_TRUE(rejections.empty());

    const auto& pool = latest.mFifo.mIsins.at(0);

    EXPECT_EQ(pool.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(pool.mOpenLots.size(), 1U);
    EXPECT_EQ(pool.mOpenLots.at(0).mState.mRemainingUnits, 500'000'000); // 5 units
    EXPECT_EQ(latest.mActions.mActions.at(0).mState, CorporateActionState::Applied);
}

TEST(CorporateActionResolverTest, AJanuarySplitRowElevenDaysAfterTheSaleIsMissingHistory) {
    // 10 bought, 15 sold on 2024-12-28, split row on 2025-01-08, 11 days after the sale: too late
    // to have taken effect before it, so purchases are missing.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-28"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 1'500'000'000,     // 15 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-08"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    EXPECT_EQ(latest.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    ASSERT_EQ(latest.mFifo.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mFifo.mDiagnostics.at(0).mMessageKey, MessageKey::IncompleteHistory);
    EXPECT_TRUE(latest.mActions.mRequests.empty());
}

TEST(CorporateActionResolverTest, CancellingTheJanuarySplitBeforeASaleIsMissingHistory) {
    // The history with the row on 2025-01-03; the user cancels: the split did not take effect
    // before the sale, so purchases are missing, as the question says.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-28"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 1'500'000'000,     // 15 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-03"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, CancelAnswer{}}});

    EXPECT_TRUE(rejections.empty());

    EXPECT_EQ(latest.mActions.mActions.at(0).mState, CorporateActionState::Cancelled);
    EXPECT_TRUE(latest.mActions.mRequests.empty());
    ASSERT_EQ(latest.mActions.mDiagnostics.size(), 1U);

    const auto& diagnostic = latest.mActions.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::IncompleteHistory);
    // The sale, not the split row.
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-12-28"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-sell-1")}));
}

TEST(CorporateActionResolverTest, CancellingTheSplitBeforeASaleInTheYearIsMissingHistory) {
    // 10 bought, 20 sold on 2024-06-01, split row on 2024-06-03; the user cancels.
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
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 2'000'000'000,     // 20 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, CancelAnswer{}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mActions.mDiagnostics.at(0).mMessageKey, MessageKey::IncompleteHistory);
    EXPECT_EQ(latest.mActions.mDiagnostics.at(0).mContext.mTaxDate, day("2024-06-01"));
}

TEST(CorporateActionResolverTest, CancellingTheSplitBeforeASaleOfAnEarlierYearIsMissingHistory) {
    // 10 bought on 2023-06-01, 15 sold on 2023-12-28, split row on 2024-01-03, 1 sold on
    // 2024-05-02 in the selected year 2024; the user cancels. The earlier sale gets the same
    // error as without the row.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-12-28"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 1'500'000'000,   // 15 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-05-02"),
                               .mSources = {source(5, "synthetic-sell-2")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 100'000'000,     // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-03"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, CancelAnswer{}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.mDiagnostics.size(), 1U);

    const auto& diagnostic = latest.mActions.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::IncompleteHistory);
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2023-12-28"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-sell-1")}));
    EXPECT_TRUE(latest.mActions.mRequests.empty());
}

TEST(CorporateActionResolverTest, ADecemberAndAJanuaryRowOfTwoBrokersAreAskedAsOneEvent) {
    // 10 bought, 4 sold at a loss on 2024-12-10; Trade Republic books a split on 2024-12-28,
    // Interactive Brokers on 2025-01-03. One question for both rows; the dates may reach the end
    // of the loss-analysis view, because one row is after 31 December.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-12-28"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2025-01-03"),
                               .mSources = {ibkrSource(4, "synthetic-ibkr-split-1")}},
                 .mType = CorporateActionType::Split},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mEvents.size(), 2U);
    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_TRUE(data.mAskSameEvent);
    EXPECT_EQ(data.mPositionBefore, 600'000'000); // 6 units on 2024-12-28
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-03-02"),
                                                .mLatestDate = day("2024-12-10"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -400'000'000},
                                               {.mEarliestDate = day("2024-12-11"),
                                                .mLatestDate = day("2025-01-30"),
                                                .mPositionBefore = 600'000'000}}));
}

TEST(CorporateActionResolverTest, TwoBrokerRowsWithAMergerAreNotAskedAsOneSplit) {
    // Trade Republic books a split on 2024-06-01; Interactive Brokers has a split and a merger row
    // on 2024-06-05. Rows with a merger are never one split: the split blocks as ambiguous, and
    // the merger is unsupported.
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
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-06-05"),
                               .mSources = {ibkrSource(3, "synthetic-ibkr-split-1")}},
                 .mType = CorporateActionType::Split},
                {.mMetadata = {.mTaxDate = day("2024-06-05"),
                               .mSources = {ibkrSource(4, "synthetic-ibkr-merger-1")}},
                 .mType = CorporateActionType::Merger},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    EXPECT_TRUE(result.mRequests.empty());
    ASSERT_EQ(result.mActions.size(), 2U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::DifferentDatesAmbiguous);
    EXPECT_EQ(result.mActions.at(1).mState, CorporateActionState::Unsupported);
}

TEST(CorporateActionResolverTest, AJanuaryPurchaseBeforeTheJanuaryRowCountsInItsPosition) {
    // 10 bought, 4 sold at a loss on 2024-12-10, 2 bought on 2025-01-03, split row on 2025-01-05:
    // 6 + 2 = 8 units just before the row.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-03"),
                               .mSources = {source(4, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 400'000'000, // EUR 4
                 .mUnits = 200'000'000,     // 2 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-05"),
                               .mSources = {source(5, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mPositionBefore, 800'000'000); // 8 units
    EXPECT_EQ(data.mDateRanges.back().mEarliestDate, day("2025-01-04"));
    EXPECT_EQ(data.mDateRanges.back().mLatestDate, day("2025-01-30"));
    EXPECT_EQ(data.mDateRanges.back().mPositionBefore, 800'000'000);
}

TEST(CorporateActionResolverTest, AJanuaryRowAfterAnOpenSplitOfTheYearIsLeftOut) {
    // A split row of 2024-11-01 is asked; its dates end on 31 December, and the January row after
    // it is neither asked nor listed as waiting.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-11-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2025-01-05"),
                               .mSources = {source(5, "synthetic-split-2")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mEffectiveDate, day("2024-11-01"));
    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mDateRanges.back().mLatestDate, day("2024-12-31"));
}

TEST(CorporateActionResolverTest, AJanuaryMergerInALossWindowIsNotAsked) {
    // A merger row is never asked; the loss stage blocks the December loss sale instead.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-05"),
                               .mSources = {source(4, "synthetic-merger-1")}},
                 .mType = CorporateActionType::Merger},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    EXPECT_TRUE(result.mActions.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(CorporateActionResolverTest, AJanuarySplitAfterALossPastTheHoldingPeriodIsNotAsked) {
    // Bought in 2008 and sold at a loss on 2024-12-10, more than 15 years later: the loss is tax
    // free and not checked, so the January row is not needed.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-05"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    EXPECT_TRUE(result.mActions.empty());
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(CorporateActionResolverTest, AJanuarySplitAfterALossOfABondIsNotAsked) {
    // A bond never reaches Doh-KDVP, so the loss stage does not check its December loss and the
    // January row is not needed: no question and no error for the row.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Bond,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-05"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    EXPECT_TRUE(result.mActions.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(CorporateActionResolverTest, ADecemberAndAJanuaryRowAnsweredAsOneSplitAreUsedOnce) {
    // 10 bought, 4 sold at a loss on 2024-12-10, split rows of Trade Republic on 2024-12-28 and
    // Interactive Brokers on 2025-01-03; the user says they are one split, 2 : 1 from 2025-01-03.
    // The split is kept once for the loss rule (one later action with both rows), and the 6 units
    // at 31 December stay 6 (not 12 or 24).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-12-28"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2025-01-03"),
                               .mSources = {ibkrSource(4, "synthetic-ibkr-split-1")}},
                 .mType = CorporateActionType::Split},
            },
    }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    // 6 units before the split on 2025-01-03, 12 after it.
    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2025-01-03"),
                                                              .mConfirmedUnits = 1'200'000'000}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.mActions.size(), 1U);
    EXPECT_EQ(latest.mActions.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(latest.mActions.mActions.at(0).mEvents.size(), 2U);

    EXPECT_TRUE(latest.mActions.mRequests.empty());
    // Only the warning that the user supplied the ratio, once.
    ASSERT_EQ(latest.mActions.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mActions.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAppliedUserRatio);

    const auto& pool = latest.mFifo.mIsins.at(0);

    EXPECT_EQ(pool.mStatus, FifoIsinStatus::Matched);
    EXPECT_TRUE(pool.mAppliedActions.empty());
    ASSERT_EQ(pool.mLaterActions.size(), 1U);
    EXPECT_EQ(pool.mLaterActions.at(0).mEvents.size(), 2U);
    EXPECT_EQ(pool.mLaterActions.at(0).mEffectiveDate, day("2025-01-03"));
    ASSERT_EQ(pool.mOpenLots.size(), 1U);
    EXPECT_EQ(pool.mOpenLots.at(0).mState.mRemainingUnits, 600'000'000); // 6 units
}

TEST(CorporateActionResolverTest, ADecemberAndAJanuaryRowNineteenDaysApartAreTwoActions) {
    // Trade Republic books a split on 2024-12-15, Interactive Brokers on 2025-01-03: 19 days
    // apart, more than 10, so they are two actions. The December row is asked alone, with dates
    // up to 31 December; the January row after it is left out of this run.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-12-15"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2025-01-03"),
                               .mSources = {ibkrSource(4, "synthetic-ibkr-split-1")}},
                 .mType = CorporateActionType::Split},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mEvents.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::WaitingForRatio);
    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(result.mRequests.at(0).mQuestion.mData);

    EXPECT_FALSE(data.mAskSameEvent);
    EXPECT_EQ(data.mSources.size(), 1U);
    EXPECT_EQ(data.mPositionBefore, 600'000'000); // 6 units on 2024-12-15
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-03-02"),
                                                .mLatestDate = day("2024-12-10"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -400'000'000},
                                               {.mEarliestDate = day("2024-12-11"),
                                                .mLatestDate = day("2024-12-31"),
                                                .mPositionBefore = 600'000'000}}));
}

TEST(CorporateActionResolverTest, ASecondJanuaryRowCountsTheFirstJanuarySplit) {
    // 10 bought, 4 sold at a loss on 2024-12-20 (window up to 2025-01-19), split rows on
    // 2025-01-05 and 2025-01-15, 4 bought on 2025-01-08. The first is answered 2 : 1 from
    // 2025-01-05; it never changes the lots at 31 December, but the second question counts it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 1'000'000'000,     // 10 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-12-20"),
                                       .mSources = {source(3, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 800'000'000, // EUR 8
                         .mUnits = 400'000'000,     // 4 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2025-01-08"),
                                       .mSources = {source(5, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 400'000'000, // EUR 4
                         .mUnits = 400'000'000,     // 4 units
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2025-01-05"),
                                       .mSources = {source(4, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                        {.mMetadata = {.mTaxDate = day("2025-01-15"),
                                       .mSources = {source(6, "synthetic-split-2")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                    },
            }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2025-01-05"),
                                                              .mConfirmedUnits = 1'200'000'000}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(latest.mActions.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mEffectiveDate, day("2025-01-15"));
    EXPECT_EQ(data.mPositionBefore, 1'600'000'000); // 6 x 2 / 1 + 4 units
    // After the first split: 12 up to the January purchase, then 16.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2025-01-06"),
                                                .mLatestDate = day("2025-01-08"),
                                                .mPositionBefore = 1'200'000'000},
                                               {.mEarliestDate = day("2025-01-09"),
                                                .mLatestDate = day("2025-01-30"),
                                                .mPositionBefore = 1'600'000'000}}));
}

TEST(CorporateActionResolverTest, AJanuarySaleBetweenTwoJanuarySplitsCountsInTheirUnits) {
    // 10 bought, 4 sold at a loss on 2024-12-20, split rows on 2025-01-05 and 2025-01-15, 10 sold
    // on 2025-01-08. The first row is answered 2 : 1 from 2025-01-05: 12 units after it, so 2 are
    // left before the second row.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 1'000'000'000,     // 10 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-12-20"),
                                       .mSources = {source(3, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 800'000'000, // EUR 8
                         .mUnits = 400'000'000,     // 4 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2025-01-08"),
                                       .mSources = {source(5, "synthetic-sell-2")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 400'000'000, // EUR 4
                         .mUnits = 1'000'000'000,   // 10 units
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2025-01-05"),
                                       .mSources = {source(4, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                        {.mMetadata = {.mTaxDate = day("2025-01-15"),
                                       .mSources = {source(6, "synthetic-split-2")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                    },
            }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2025-01-05"),
                                                              .mConfirmedUnits = 1'200'000'000}}});

    EXPECT_TRUE(rejections.empty());

    ASSERT_EQ(latest.mActions.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(latest.mActions.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mPositionBefore, 200'000'000); // 6 x 2 / 1 - 10 units
    // 12 up to the January sale, which comes after a date of that range (change -10), then 2.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2025-01-06"),
                                                .mLatestDate = day("2025-01-08"),
                                                .mPositionBefore = 1'200'000'000,
                                                .mLowestChangeAfter = -1'000'000'000},
                                               {.mEarliestDate = day("2025-01-09"),
                                                .mLatestDate = day("2025-01-30"),
                                                .mPositionBefore = 200'000'000}}));
}

TEST(CorporateActionResolverTest, AFirstJanuarySplitWhoseUnitsDoNotFitIsANumericError) {
    // 10 bought, 4 sold at a loss on 2024-12-20, split rows on 2025-01-05 and 2025-01-15, 4 bought
    // on 2025-01-08. The first row is confirmed as 20000000000 : 1 from 2025-01-05. The resolver
    // gets it directly: the session would reject a ratio whose preview does not fit. Before the
    // January purchase the 6 units would be 120000000000 units, above the largest Units value
    // (about 92233720368), so the second row cannot be asked.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-03-01"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 1'000'000'000,     // 10 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-12-20"),
                                       .mSources = {source(3, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 800'000'000, // EUR 8
                         .mUnits = 400'000'000,     // 4 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2025-01-08"),
                                       .mSources = {source(5, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 400'000'000, // EUR 4
                         .mUnits = 400'000'000,     // 4 units
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2025-01-05"),
                                       .mSources = {source(4, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                        {.mMetadata = {.mTaxDate = day("2025-01-15"),
                                       .mSources = {source(6, "synthetic-split-2")}},
                         .mType = CorporateActionType::UnresolvedSplit},
                    },
            }};
    const std::vector<ConfirmedCorporateAction> confirmed{
        {.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
         .mRatio = {.mNewShares = 20'000'000'000, .mOldShares = 1},
         .mEffectiveDate = day("2025-01-05")}};

    orderChronologically(merged.mStatement);
    const auto prepared = TaxProcessor{}.prepareReportingYear({2024, merged});
    const auto& ledger = prepared.mLedger.value();
    RunDecisions decisions;

    const auto fifo = FifoMatcher::match(merged.mStatement, ledger, confirmed);
    const auto result =
        CorporateActionResolver::resolve(merged.mStatement, ledger, fifo, confirmed, decisions);

    ASSERT_EQ(result.mActions.size(), 2U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(result.mActions.at(1).mState, CorporateActionState::NumericFailure);
    EXPECT_TRUE(result.mRequests.empty());

    // The warning of the first split, then the error of the second row.
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAppliedUserRatio);

    const auto& diagnostic = result.mDiagnostics.at(1);

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionUnrepresentable);
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2025-01-15"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(6, "synthetic-split-2")}));
}

TEST(CorporateActionResolverTest, AFirstJanuarySplitWhoseUnitsDoNotFitBeforeTheRowIsANumericError) {
    // 10 bought, 4 sold at a loss on 2024-12-20, split rows on 2025-01-05 and 2025-01-15, the first
    // confirmed as 20000000000 : 1 from 2025-01-05, and no January trade: the split is counted
    // after the last trade, just before the second row, and its 120000000000 units do not fit.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-05"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2025-01-15"),
                               .mSources = {source(6, "synthetic-split-2")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    const std::vector<ConfirmedCorporateAction> confirmed{
        {.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
         .mRatio = {.mNewShares = 20'000'000'000, .mOldShares = 1},
         .mEffectiveDate = day("2025-01-05")}};

    orderChronologically(merged.mStatement);
    const auto prepared = TaxProcessor{}.prepareReportingYear({2024, merged});
    const auto& ledger = prepared.mLedger.value();
    RunDecisions decisions;

    const auto fifo = FifoMatcher::match(merged.mStatement, ledger, confirmed);
    const auto result =
        CorporateActionResolver::resolve(merged.mStatement, ledger, fifo, confirmed, decisions);

    ASSERT_EQ(result.mActions.size(), 2U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::Applied);
    EXPECT_EQ(result.mActions.at(1).mState, CorporateActionState::NumericFailure);
    EXPECT_TRUE(result.mRequests.empty());

    // The warning of the first split, then the error of the second row.
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAppliedUserRatio);

    const auto& diagnostic = result.mDiagnostics.at(1);

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionUnrepresentable);
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2025-01-15"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(6, "synthetic-split-2")}));
}

TEST(CorporateActionResolverTest, AJanuarySaleWithTooFewUnitsAsksTheJanuarySplitBeforeIt) {
    // 10 bought, 4 sold at a loss on 2024-12-10 (window up to 2025-01-09), 12 sold on
    // 2025-01-03, split row on 2025-01-05. Only 6 units are left before the January sale, so the
    // broker may have booked the split after it took effect: the question ends at that sale.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-03"),
                               .mSources = {source(4, "synthetic-sell-2")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 400'000'000, // EUR 4
                 .mUnits = 1'200'000'000,   // 12 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-05"),
                               .mSources = {source(5, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mActions.mRequests.size(), 1U);

    const auto& data =
        std::get<CorporateActionRatioQuestion>(latest.mActions.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mEffectiveDate, day("2025-01-05"));
    EXPECT_FALSE(data.mPositionBefore.has_value());
    EXPECT_EQ(data.mEarlierSaleDate, day("2025-01-03"));
    // 10 up to the December sale, 6 up to the January sale; both sales come after the action.
    EXPECT_EQ(data.mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-03-02"),
                                                .mLatestDate = day("2024-12-10"),
                                                .mPositionBefore = 1'000'000'000,
                                                .mLowestChangeAfter = -1'600'000'000},
                                               {.mEarliestDate = day("2024-12-11"),
                                                .mLatestDate = day("2025-01-03"),
                                                .mPositionBefore = 600'000'000,
                                                .mLowestChangeAfter = -1'200'000'000}}));
    ASSERT_EQ(latest.mActions.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mActions.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAmbiguousRatioRequiredBeforeSale);

    // 2 : 1 from 2025-01-02 makes 12 units, which the January sale takes. The date is after
    // 31 December, so the lots at 31 December keep 6 units.
    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2025-01-02"),
                                                              .mConfirmedUnits = 1'200'000'000}}});

    EXPECT_TRUE(rejections.empty());

    EXPECT_TRUE(latest.mActions.mRequests.empty());
    EXPECT_EQ(latest.mActions.mActions.at(0).mState, CorporateActionState::Applied);

    const auto& pool = latest.mFifo.mIsins.at(0);

    ASSERT_EQ(pool.mLaterActions.size(), 1U);
    EXPECT_EQ(pool.mLaterActions.at(0).mEffectiveDate, day("2025-01-02"));
    ASSERT_EQ(pool.mOpenLots.size(), 1U);
    EXPECT_EQ(pool.mOpenLots.at(0).mState.mRemainingUnits, 600'000'000); // 6 units
}

TEST(CorporateActionResolverTest, AJanuarySaleElevenDaysBeforeTheRowIsAMissingPosition) {
    // 10 bought, 4 sold at a loss on 2024-12-20 (window up to 2025-01-19), 12 sold on
    // 2025-01-03, split row on 2025-01-14, 11 days later: purchases are missing, not asked.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-03"),
                               .mSources = {source(4, "synthetic-sell-2")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 400'000'000, // EUR 4
                 .mUnits = 1'200'000'000,   // 12 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-14"),
                               .mSources = {source(5, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result = resolveYear(merged, 2024);

    ASSERT_EQ(result.mActions.size(), 1U);
    EXPECT_EQ(result.mActions.at(0).mState, CorporateActionState::NoPosition);
    EXPECT_TRUE(result.mRequests.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionMissingPosition);
}

TEST(CorporateActionResolverTest, CancellingTheJanuarySplitBeforeAJanuarySaleIsAPlainCancel) {
    // 10 bought, 4 sold at a loss on 2024-12-10, 12 sold on 2025-01-03, split row on 2025-01-05;
    // the user cancels the question before the January sale. That sale is not in the 2024 report,
    // so the cancel gives the plain cancel error, not its missing history (maintainer decision,
    // 2026-10-30).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-03"),
                               .mSources = {source(4, "synthetic-sell-2")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 400'000'000, // EUR 4
                 .mUnits = 1'200'000'000,   // 12 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-05"),
                               .mSources = {source(5, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};
    CapitalPools latest;
    DecisionSession session;
    session.start(poolsCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, CancelAnswer{}}});

    EXPECT_TRUE(rejections.empty());

    EXPECT_EQ(latest.mActions.mActions.at(0).mState, CorporateActionState::Cancelled);
    ASSERT_EQ(latest.mActions.mDiagnostics.size(), 1U);

    const auto& diagnostic = latest.mActions.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionAmbiguous);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionAmbiguousCancelled);
    // The split row, not the January sale.
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2025-01-05"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(5, "synthetic-split-1")}));
}

} // namespace
