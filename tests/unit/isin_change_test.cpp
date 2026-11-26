#include "processors/isin_change.hpp"
#include "processors/statement_merger.hpp"
#include "processors/tax_processor.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

constexpr auto kOldIsin = "XX0000000001";
constexpr auto kNewIsin = "XX0000000002";
constexpr auto kThirdIsin = "XX0000000003";

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

/// The exact source text of a `shares` value, as the parser keeps it.
std::vector<UnitSourceEvidence> sharesEvidence(const SourceReference& aSource,
                                               const std::string& aShares) {
    return {UnitSourceEvidence{
        .mSource = aSource,
        .mSourceText = aShares,
        .mCanonicalValue = aShares,
    }};
}

/// One calculation of a session: prepares the year from the statement and applies the ISIN
/// changes with the session's answers. aLatest keeps the result of the latest calculation.
RunCalculation
changeCalculation(StatementMergeResult& aMerged, int aYear, IsinChangeResult& aLatest) {
    rebuildChronologicalOrder(aMerged.mStatement);

    return [&aMerged, aYear, &aLatest](RunDecisions& aDecisions) {
        const auto prepared = TaxProcessor::prepareReportingYear({aYear, aMerged});

        aLatest = applyIsinChanges(aMerged.mStatement, prepared.mLedger.value(), aDecisions);

        ApplicationResultInput input{.mReportingYear = aYear};
        auto diagnostics = aLatest.mDiagnostics;
        auto requests = aLatest.mRequests;

        appendStageOutput(input, std::move(diagnostics), std::move(requests));

        return input;
    };
}

/// The statement of the usual case: 10 units of the old ISIN bought on 2023-05-02, an ISIN change
/// on 2024-06-03 (10 units out, 20 units in, one action ID) and 20 units of the new ISIN sold on
/// 2024-09-02. aOutAction and aInAction are the two rows, so a test can change them.
StatementMergeResult usualStatement(CorporateAction aOutAction, CorporateAction aInAction) {
    StatementMergeResult merged;

    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Old Share",
            .mIsin = kOldIsin,
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-05-02"),
                                   .mSources = {source(2, "buy-old")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions = {std::move(aOutAction)},
        },
        {
            .mName = "Synthetic New Share",
            .mIsin = kNewIsin,
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-09-02"),
                                   .mSources = {source(5, "sell-new")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'500'000'000, // EUR 15
                     .mUnits = 2'000'000'000,     // 20 units
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions = {std::move(aInAction)},
        },
    };

    return merged;
}

CorporateAction usualOutAction() {
    return {.mMetadata = {.mTaxDate = day("2024-06-03"), .mSources = {source(3, "out-1")}},
            .mType = CorporateActionType::IsinChange,
            .mUnitsDelta = -1'000'000'000, // 10 units out
            .mBrokerActionId = "change-1",
            .mNewIsin = kNewIsin,
            .mUnitEvidence = sharesEvidence(source(3, "out-1"), "-10")};
}

CorporateAction usualInAction() {
    return {.mMetadata = {.mTaxDate = day("2024-06-03"), .mSources = {source(4, "in-1")}},
            .mType = CorporateActionType::IsinChange,
            .mUnitsDelta = 2'000'000'000, // 20 units in
            .mBrokerActionId = "change-1",
            .mUnitEvidence = sharesEvidence(source(4, "in-1"), "20")};
}

const AnswerValue kSameIssuerTwoForOne = IsinChangeAnswer{
    .mSameIssuer = true, .mNewShares = 2, .mOldShares = 1, .mEffectiveDate = day("2024-06-03")};

TEST(IsinChangeTest, AStatementWithoutAnIsinChangeIsNotChanged) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Old Share",
        .mIsin = kOldIsin,
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-09-02"), .mSources = {source(2, "sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'500'000'000, // EUR 15
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03"), .mSources = {source(3, "split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 1'000'000'000},
            },
    }};
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    EXPECT_FALSE(latest.mStatement.has_value());
    EXPECT_TRUE(latest.mDiagnostics.empty());
    EXPECT_TRUE(latest.mRequests.empty());
}

TEST(IsinChangeTest, AnIsinChangeOfASoldInvestmentAsksOneQuestionAndBlocksBothIsins) {
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    EXPECT_FALSE(latest.mStatement.has_value());

    ASSERT_EQ(latest.mDiagnostics.size(), 2U);

    // Both ISINs get the same error, the old one first.
    const auto& oldError = latest.mDiagnostics.at(0);
    const auto& newError = latest.mDiagnostics.at(1);

    EXPECT_EQ(oldError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(oldError.mCode, ProcessingDiagnosticCode::CorporateActionAmbiguous);
    EXPECT_EQ(oldError.mMessageKey, MessageKey::CorporateActionAmbiguousIsinChangeRequired);
    EXPECT_EQ(oldError.mContext.mStage, PipelineStage::CorporateAction);
    EXPECT_EQ(oldError.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(oldError.mContext.mIsin, kOldIsin);
    EXPECT_EQ(oldError.mContext.mInstrumentName, "Synthetic Old Share");
    EXPECT_EQ(oldError.mContext.mTaxDate, day("2024-06-03"));
    // The sources of both rows of the action.
    EXPECT_EQ(oldError.mContext.mSources,
              (std::vector<SourceReference>{source(3, "out-1"), source(4, "in-1")}));

    EXPECT_EQ(newError.mCode, ProcessingDiagnosticCode::CorporateActionAmbiguous);
    EXPECT_EQ(newError.mContext.mIsin, kNewIsin);
    EXPECT_EQ(newError.mContext.mInstrumentName, "Synthetic New Share");

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& request = latest.mRequests.at(0);
    const auto& question = request.mQuestion;

    EXPECT_EQ(request.mProcessingDiagnostics, (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(question.mType, RequestType::CorporateActionIsinChangeRequired);
    EXPECT_EQ(question.mAffectedReports, (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(question.mIsin, kOldIsin);
    EXPECT_EQ(question.mTaxDate, day("2024-06-03"));

    const auto& data = std::get<IsinChangeQuestion>(question.mData);

    EXPECT_EQ(data.mOldIsin, kOldIsin);
    EXPECT_EQ(data.mOldName, "Synthetic Old Share");
    EXPECT_EQ(data.mNewIsin, kNewIsin);
    EXPECT_EQ(data.mNewName, "Synthetic New Share");
    EXPECT_EQ(data.mEffectiveDate, day("2024-06-03"));
    EXPECT_EQ(data.mEarliestDate, day("2024-06-03"));
    EXPECT_EQ(data.mLatestDate, day("2024-06-03"));
    // 10 units left and 20 came in: 2 new for 1 old.
    EXPECT_EQ(data.mSuggestedRatio, (SplitRatio{.mNewShares = 2, .mOldShares = 1}));
    EXPECT_EQ(data.mSources,
              (std::vector<CorporateActionSourceFacts>{{.mBroker = Broker::TradeRepublic,
                                                        .mTransactionId = "out-1",
                                                        .mDate = day("2024-06-03"),
                                                        .mSourceValue = "-10"},
                                                       {.mBroker = Broker::TradeRepublic,
                                                        .mTransactionId = "in-1",
                                                        .mDate = day("2024-06-03"),
                                                        .mSourceValue = "20"}}));
}

TEST(IsinChangeTest, ASaleOnTheLastDayOfTheYearMakesAnIsinChangeMatter) {
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());

    merged.mStatement.mPresentation.mTradeInstruments.at(1).mTransactions.at(0).mMetadata.mTaxDate =
        day("2024-12-31");

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    EXPECT_EQ(latest.mRequests.size(), 1U);
}

TEST(IsinChangeTest, ASaleOnTheFirstDayOfTheNextYearDoesNotMakeAnIsinChangeMatter) {
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());

    merged.mStatement.mPresentation.mTradeInstruments.at(1).mTransactions.at(0).mMetadata.mTaxDate =
        day("2025-01-01");

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    EXPECT_FALSE(latest.mStatement.has_value());
    EXPECT_TRUE(latest.mDiagnostics.empty());
    EXPECT_TRUE(latest.mRequests.empty());
}

TEST(IsinChangeTest, ASaleOnTheLastDayBeforeTheYearDoesNotMakeAnIsinChangeMatter) {
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());

    merged.mStatement.mPresentation.mTradeInstruments.at(1).mTransactions.at(0).mMetadata.mTaxDate =
        day("2023-12-31");

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    EXPECT_FALSE(latest.mStatement.has_value());
    EXPECT_TRUE(latest.mDiagnostics.empty());
    EXPECT_TRUE(latest.mRequests.empty());
}

TEST(IsinChangeTest, ASaleOfTheOldIsinOnlyMakesAnIsinChangeMatterToo) {
    // The sale of the year is of the old ISIN; the new ISIN has no sale.
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());
    auto& instruments = merged.mStatement.mPresentation.mTradeInstruments;

    instruments.at(0).mTransactions.push_back(
        {.mMetadata = {.mTaxDate = day("2024-02-01"), .mSources = {source(6, "sell-old")}},
         .mTradeSide = TradeSide::Sell,
         .mUnitPrice = 1'200'000'000, // EUR 12
         .mUnits = 500'000'000,       // 5 units
         .mCurrency = Currency::EUR});
    instruments.at(1).mTransactions.clear();

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    EXPECT_EQ(latest.mRequests.size(), 1U);
}

TEST(IsinChangeTest, AnIsinChangeUpToTheEndOfTheLossViewIsAsked) {
    // The loss view of 2024 ends 30 days after 31 December: 2025-01-30.
    CorporateAction out = usualOutAction();
    CorporateAction in = usualInAction();

    out.mMetadata.mTaxDate = day("2025-01-30");
    in.mMetadata.mTaxDate = day("2025-01-30");

    StatementMergeResult merged = usualStatement(std::move(out), std::move(in));
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    EXPECT_EQ(latest.mRequests.size(), 1U);
}

TEST(IsinChangeTest, AnIsinChangeAfterTheEndOfTheLossViewIsNotAsked) {
    // 2025-01-31 is one day after the loss view of 2024.
    CorporateAction out = usualOutAction();
    CorporateAction in = usualInAction();

    out.mMetadata.mTaxDate = day("2025-01-31");
    in.mMetadata.mTaxDate = day("2025-01-31");

    StatementMergeResult merged = usualStatement(std::move(out), std::move(in));
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    EXPECT_FALSE(latest.mStatement.has_value());
    EXPECT_TRUE(latest.mDiagnostics.empty());
    EXPECT_TRUE(latest.mRequests.empty());
}

TEST(IsinChangeTest, SameIssuerMovesTheLotsAndAddsTheSplitOfTheAnswer) {
    // 10 units out and 20 in, answered 2 new for 1 old on 2024-06-03.
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, kSameIssuerTwoForOne}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const auto& instruments = latest.mStatement->mPresentation.mTradeInstruments;

    // The old instrument is gone; the new one has its lots.
    ASSERT_EQ(instruments.size(), 1U);

    const auto& instrument = instruments.at(0);

    EXPECT_EQ(instrument.mIsin, kNewIsin);
    EXPECT_EQ(instrument.mName, "Synthetic New Share");

    // The purchase of the old ISIN comes first and stays listed under the old ISIN and name.
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    EXPECT_EQ(instrument.mTransactions.at(0).mMetadata.mTaxDate, day("2023-05-02"));
    EXPECT_EQ(instrument.mTransactions.at(0).mTradeSide, TradeSide::Buy);
    EXPECT_EQ(instrument.mTransactions.at(0).mListedAs,
              (ListedInstrument{.mIsin = kOldIsin, .mName = "Synthetic Old Share"}));
    EXPECT_EQ(instrument.mTransactions.at(1).mMetadata.mTaxDate, day("2024-09-02"));
    EXPECT_FALSE(instrument.mTransactions.at(1).mListedAs.has_value());

    // The two rows of the change are one confirmed split row with the sources of both.
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.at(0);

    EXPECT_EQ(action.mType, CorporateActionType::Split);
    EXPECT_EQ(action.mRatio, (SplitRatio{.mNewShares = 2, .mOldShares = 1}));
    EXPECT_FALSE(action.mNewIsin.has_value());
    EXPECT_EQ(action.mMetadata.mTaxDate, day("2024-06-03"));
    EXPECT_EQ(action.mMetadata.mSources,
              (std::vector<SourceReference>{source(4, "in-1"), source(3, "out-1")}));

    // The new statement has its order again: 2 trades and 1 action.
    EXPECT_EQ(latest.mStatement->mChronologicalOrder.size(), 3U);

    ASSERT_EQ(latest.mDiagnostics.size(), 1U);

    const auto& diagnostic = latest.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionApplied);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionAppliedIsinChange);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{textParam("oldIsin", kOldIsin),
                                         textParam("newIsin", kNewIsin),
                                         numberParam("newShares", 2),
                                         numberParam("oldShares", 1),
                                         textParam("effectiveDate", "2024-06-03")}));
    EXPECT_EQ(diagnostic.mContext.mIsin, kNewIsin);
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-06-03"));
}

TEST(IsinChangeTest, SameIssuerWithEqualUnitsLeavesNoActionRow) {
    // 10 units out, 10 units in: nothing is multiplied, so no split row is made.
    CorporateAction in = usualInAction();

    in.mUnitsDelta = 1'000'000'000; // 10 units in
    in.mUnitEvidence = sharesEvidence(source(4, "in-1"), "10");

    StatementMergeResult merged = usualStatement(usualOutAction(), std::move(in));
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    const auto rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 1,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-06-03")}}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const auto& instruments = latest.mStatement->mPresentation.mTradeInstruments;

    ASSERT_EQ(instruments.size(), 1U);
    EXPECT_EQ(instruments.at(0).mIsin, kNewIsin);
    EXPECT_TRUE(instruments.at(0).mCorporateActions.empty());
    EXPECT_EQ(instruments.at(0).mTransactions.size(), 2U);
    EXPECT_EQ(latest.mStatement->mChronologicalOrder.size(), 2U);

    ASSERT_EQ(latest.mDiagnostics.size(), 1U);
    EXPECT_EQ(latest.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAppliedIsinChange);
    EXPECT_EQ(latest.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("oldIsin", kOldIsin),
                                         textParam("newIsin", kNewIsin),
                                         numberParam("newShares", 1),
                                         numberParam("oldShares", 1),
                                         textParam("effectiveDate", "2024-06-03")}));
}

TEST(IsinChangeTest, SameIssuerWithFewerUnitsInIsAReverseSplit) {
    // 20 units out and 10 in: 1 new for 2 old.
    CorporateAction out = usualOutAction();
    CorporateAction in = usualInAction();

    out.mUnitsDelta = -2'000'000'000; // 20 units out
    out.mUnitEvidence = sharesEvidence(source(3, "out-1"), "-20");
    in.mUnitsDelta = 1'000'000'000; // 10 units in
    in.mUnitEvidence = sharesEvidence(source(4, "in-1"), "10");

    StatementMergeResult merged = usualStatement(std::move(out), std::move(in));
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    const auto rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 1,
                                          .mOldShares = 2,
                                          .mEffectiveDate = day("2024-06-03")}}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const auto& actions =
        latest.mStatement->mPresentation.mTradeInstruments.at(0).mCorporateActions;

    ASSERT_EQ(actions.size(), 1U);
    EXPECT_EQ(actions.at(0).mType, CorporateActionType::ReverseSplit);
    EXPECT_EQ(actions.at(0).mRatio, (SplitRatio{.mNewShares = 1, .mOldShares = 2}));
}

TEST(IsinChangeTest, AnEffectiveDateOtherThanTheRowsDateHasNoTimeOfDay) {
    // The rows are on 2024-06-03 (out) and 2024-06-05 (in, with a time). The answer is
    // 2024-06-04, so the kept row gets that date and loses its time.
    CorporateAction out = usualOutAction();
    CorporateAction in = usualInAction();

    in.mMetadata.mTaxDate = day("2024-06-05");
    in.mMetadata.mSourceTimestamp = SourceTimestamp{
        std::chrono::sys_days{std::chrono::year{2024} / 6 / 5} + std::chrono::hours{10}};
    in.mMetadata.mOrderingTimestamp = in.mMetadata.mSourceTimestamp;

    StatementMergeResult merged = usualStatement(std::move(out), std::move(in));
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data = std::get<IsinChangeQuestion>(latest.mRequests.at(0).mQuestion.mData);

    // The question is pre-filled with the date of the row of the old ISIN, between the two dates.
    EXPECT_EQ(data.mEffectiveDate, day("2024-06-03"));
    EXPECT_EQ(data.mEarliestDate, day("2024-06-03"));
    EXPECT_EQ(data.mLatestDate, day("2024-06-05"));

    const auto rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 2,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-06-04")}}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const auto& actions =
        latest.mStatement->mPresentation.mTradeInstruments.at(0).mCorporateActions;

    ASSERT_EQ(actions.size(), 1U);
    EXPECT_EQ(actions.at(0).mMetadata.mTaxDate, day("2024-06-04"));
    EXPECT_FALSE(actions.at(0).mMetadata.mSourceTimestamp.has_value());
    EXPECT_FALSE(actions.at(0).mMetadata.mOrderingTimestamp.has_value());
}

TEST(IsinChangeTest, ASplitRowOfTheNewIsinAfterTheChangeStaysAfterTheSplitOfTheAnswer) {
    // The new ISIN has an own split row on 2024-08-01, after the change of 2024-06-03. The split
    // made from the answer is put before it, by date.
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());

    merged.mStatement.mPresentation.mTradeInstruments.at(1).mCorporateActions.push_back(
        {.mMetadata = {.mTaxDate = day("2024-08-01"), .mSources = {source(6, "split-new")}},
         .mType = CorporateActionType::UnresolvedSplit,
         .mUnitsDelta = 100'000'000});

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, kSameIssuerTwoForOne}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const auto& actions =
        latest.mStatement->mPresentation.mTradeInstruments.at(0).mCorporateActions;

    ASSERT_EQ(actions.size(), 2U);
    EXPECT_EQ(actions.at(0).mType, CorporateActionType::Split);
    EXPECT_EQ(actions.at(0).mMetadata.mTaxDate, day("2024-06-03"));
    EXPECT_EQ(actions.at(1).mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(actions.at(1).mMetadata.mTaxDate, day("2024-08-01"));
}

TEST(IsinChangeTest, AnEffectiveDateEqualToTheRowsDateKeepsItsTimeOfDay) {
    CorporateAction in = usualInAction();

    in.mMetadata.mSourceTimestamp = SourceTimestamp{
        std::chrono::sys_days{std::chrono::year{2024} / 6 / 3} + std::chrono::hours{10}};

    StatementMergeResult merged = usualStatement(usualOutAction(), std::move(in));
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, kSameIssuerTwoForOne}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const auto& actions =
        latest.mStatement->mPresentation.mTradeInstruments.at(0).mCorporateActions;

    ASSERT_EQ(actions.size(), 1U);
    EXPECT_EQ(actions.at(0).mMetadata.mSourceTimestamp,
              (SourceTimestamp{std::chrono::sys_days{std::chrono::year{2024} / 6 / 3} +
                               std::chrono::hours{10}}));
}

TEST(IsinChangeTest, TheRowsOfTwoAccountsAreOneChangeWithTheSumOfTheirUnits) {
    // Two accounts: 4 + 6 units out, 8 + 12 units in, one broker action ID. 10 out and 20 in is
    // 2 / 1. The kept row has the sources of all four rows.
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());
    auto& instruments = merged.mStatement.mPresentation.mTradeInstruments;

    instruments.at(0).mCorporateActions = {
        {.mMetadata = {.mTaxDate = day("2024-06-03"), .mSources = {source(3, "out-a")}},
         .mType = CorporateActionType::IsinChange,
         .mUnitsDelta = -400'000'000, // 4 units out
         .mBrokerActionId = "change-1",
         .mNewIsin = kNewIsin,
         .mUnitEvidence = sharesEvidence(source(3, "out-a"), "-4")},
        {.mMetadata = {.mTaxDate = day("2024-06-03"), .mSources = {source(4, "out-b")}},
         .mType = CorporateActionType::IsinChange,
         .mUnitsDelta = -600'000'000, // 6 units out
         .mBrokerActionId = "change-1",
         .mNewIsin = kNewIsin,
         .mUnitEvidence = sharesEvidence(source(4, "out-b"), "-6")},
    };
    instruments.at(1).mCorporateActions = {
        {.mMetadata = {.mTaxDate = day("2024-06-03"), .mSources = {source(7, "in-a")}},
         .mType = CorporateActionType::IsinChange,
         .mUnitsDelta = 800'000'000, // 8 units in
         .mBrokerActionId = "change-1",
         .mUnitEvidence = sharesEvidence(source(7, "in-a"), "8")},
        {.mMetadata = {.mTaxDate = day("2024-06-03"), .mSources = {source(8, "in-b")}},
         .mType = CorporateActionType::IsinChange,
         .mUnitsDelta = 1'200'000'000, // 12 units in
         .mBrokerActionId = "change-1",
         .mUnitEvidence = sharesEvidence(source(8, "in-b"), "12")},
    };

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data = std::get<IsinChangeQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mSources.size(), 4U);
    EXPECT_EQ(data.mSuggestedRatio, (SplitRatio{.mNewShares = 2, .mOldShares = 1}));

    const auto rejections = session.answer({{"request-0001", 1, kSameIssuerTwoForOne}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const auto& actions =
        latest.mStatement->mPresentation.mTradeInstruments.at(0).mCorporateActions;

    ASSERT_EQ(actions.size(), 1U);
    EXPECT_EQ(actions.at(0).mType, CorporateActionType::Split);
    // The first row of the new ISIN is kept; the sources of the other rows are added to it.
    EXPECT_EQ(actions.at(0).mMetadata.mSources,
              (std::vector<SourceReference>{source(7, "in-a"),
                                            source(8, "in-b"),
                                            source(3, "out-a"),
                                            source(4, "out-b")}));
    EXPECT_EQ(actions.at(0).mUnitEvidence.size(), 2U);
}

TEST(IsinChangeTest, AnAnswerNoMakesTheRowsAStockMerger) {
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    const auto rejections =
        session.answer({{"request-0001", 1, IsinChangeAnswer{.mSameIssuer = false}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(latest.mRequests.empty());
    // No error and no warning here: the merger stage reports the rows.
    EXPECT_TRUE(latest.mDiagnostics.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const auto& instruments = latest.mStatement->mPresentation.mTradeInstruments;

    // Both instruments stay; the rows are retyped, the lots are not moved.
    ASSERT_EQ(instruments.size(), 2U);
    EXPECT_EQ(instruments.at(0).mIsin, kOldIsin);
    ASSERT_EQ(instruments.at(0).mCorporateActions.size(), 1U);
    EXPECT_EQ(instruments.at(0).mCorporateActions.at(0).mType, CorporateActionType::StockMerger);
    EXPECT_EQ(instruments.at(1).mIsin, kNewIsin);
    ASSERT_EQ(instruments.at(1).mCorporateActions.size(), 1U);
    EXPECT_EQ(instruments.at(1).mCorporateActions.at(0).mType, CorporateActionType::StockMerger);
    EXPECT_FALSE(instruments.at(0).mTransactions.at(0).mListedAs.has_value());
}

TEST(IsinChangeTest, ACancelledQuestionBlocksBothIsinsAndChangesNothing) {
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, CancelAnswer{}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(latest.mRequests.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
    ASSERT_EQ(latest.mDiagnostics.size(), 2U);

    EXPECT_EQ(latest.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(latest.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::CorporateActionAmbiguous);
    EXPECT_EQ(latest.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAmbiguousCancelled);
    EXPECT_EQ(latest.mDiagnostics.at(0).mContext.mIsin, kOldIsin);
    EXPECT_EQ(latest.mDiagnostics.at(1).mMessageKey, MessageKey::CorporateActionAmbiguousCancelled);
    EXPECT_EQ(latest.mDiagnostics.at(1).mContext.mIsin, kNewIsin);
}

TEST(IsinChangeTest, APurchaseOfTheNewIsinBeforeTheChangeBlocksBothIsins) {
    // The new ISIN is bought on 2024-05-01, before the change of 2024-06-03: the lots cannot be
    // put in one pool in date order.
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());

    merged.mStatement.mPresentation.mTradeInstruments.at(1).mTransactions.push_back(
        {.mMetadata = {.mTaxDate = day("2024-05-01"), .mSources = {source(6, "buy-new")}},
         .mTradeSide = TradeSide::Buy,
         .mUnitPrice = 500'000'000, // EUR 5
         .mUnits = 100'000'000,     // 1 unit
         .mCurrency = Currency::EUR});

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, kSameIssuerTwoForOne}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
    ASSERT_EQ(latest.mDiagnostics.size(), 2U);

    EXPECT_EQ(latest.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(latest.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::CorporateActionInconsistent);
    EXPECT_EQ(latest.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionInconsistentTradeAcrossIsinChange);
    EXPECT_EQ(latest.mDiagnostics.at(0).mContext.mIsin, kOldIsin);
    EXPECT_EQ(latest.mDiagnostics.at(1).mContext.mIsin, kNewIsin);
}

TEST(IsinChangeTest, ASaleOfTheOldIsinOnTheEffectiveDateBlocksBothIsins) {
    // A sale of the old ISIN on the day of the change is not before it: the date is crossed.
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());

    merged.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions.push_back(
        {.mMetadata = {.mTaxDate = day("2024-06-03"), .mSources = {source(6, "sell-old")}},
         .mTradeSide = TradeSide::Sell,
         .mUnitPrice = 1'200'000'000, // EUR 12
         .mUnits = 100'000'000,       // 1 unit
         .mCurrency = Currency::EUR});

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, kSameIssuerTwoForOne}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
    ASSERT_EQ(latest.mDiagnostics.size(), 2U);
    EXPECT_EQ(latest.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::CorporateActionInconsistent);
}

TEST(IsinChangeTest, ASplitOfTheOldIsinAfterTheChangeBlocksBothIsins) {
    // Another action row of the old ISIN after the change cannot move with the lots.
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());

    merged.mStatement.mPresentation.mTradeInstruments.at(0).mCorporateActions.push_back(
        {.mMetadata = {.mTaxDate = day("2024-07-01"), .mSources = {source(6, "split-old")}},
         .mType = CorporateActionType::UnresolvedSplit,
         .mUnitsDelta = 100'000'000});

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, kSameIssuerTwoForOne}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
    ASSERT_EQ(latest.mDiagnostics.size(), 2U);
    EXPECT_EQ(latest.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::CorporateActionInconsistent);
}

TEST(IsinChangeTest, ASplitOfTheNewIsinBeforeTheChangeBlocksBothIsins) {
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());

    merged.mStatement.mPresentation.mTradeInstruments.at(1).mCorporateActions.push_back(
        {.mMetadata = {.mTaxDate = day("2024-03-01"), .mSources = {source(6, "split-new")}},
         .mType = CorporateActionType::UnresolvedSplit,
         .mUnitsDelta = 100'000'000});

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, kSameIssuerTwoForOne}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
    ASSERT_EQ(latest.mDiagnostics.size(), 2U);
    EXPECT_EQ(latest.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::CorporateActionInconsistent);
}

TEST(IsinChangeTest, ARowWithoutAPartnerInstrumentBecomesAMerger) {
    // The new ISIN has no instrument in the statement: the row names an action the app does not
    // know. It is retyped and asks nothing.
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());

    merged.mStatement.mPresentation.mTradeInstruments.pop_back();

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    EXPECT_TRUE(latest.mDiagnostics.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const auto& instruments = latest.mStatement->mPresentation.mTradeInstruments;

    ASSERT_EQ(instruments.size(), 1U);
    ASSERT_EQ(instruments.at(0).mCorporateActions.size(), 1U);
    EXPECT_EQ(instruments.at(0).mCorporateActions.at(0).mType, CorporateActionType::Merger);
}

TEST(IsinChangeTest, ARowOfTheNewIsinWithAnotherActionIdIsNotItsPartner) {
    // The out row has the ID change-1, the in row change-2: two different actions.
    CorporateAction in = usualInAction();

    in.mBrokerActionId = "change-2";

    StatementMergeResult merged = usualStatement(usualOutAction(), std::move(in));
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    EXPECT_TRUE(latest.mDiagnostics.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const auto& instruments = latest.mStatement->mPresentation.mTradeInstruments;

    EXPECT_EQ(instruments.at(0).mCorporateActions.at(0).mType, CorporateActionType::Merger);
    // The row of the new ISIN is not retyped here: it has no partner row to name it.
    EXPECT_EQ(instruments.at(1).mCorporateActions.at(0).mType, CorporateActionType::IsinChange);
}

TEST(IsinChangeTest, RowsWithoutAnActionIdAreOneChangeWithinTenDays) {
    // No broker IDs: the in row, 10 days after the out row, is its partner.
    CorporateAction out = usualOutAction();
    CorporateAction in = usualInAction();

    out.mBrokerActionId = std::nullopt;
    in.mBrokerActionId = std::nullopt;
    in.mMetadata.mTaxDate = day("2024-06-13");

    StatementMergeResult merged = usualStatement(std::move(out), std::move(in));
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data = std::get<IsinChangeQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(data.mEarliestDate, day("2024-06-03"));
    EXPECT_EQ(data.mLatestDate, day("2024-06-13"));
    EXPECT_EQ(data.mSources.size(), 2U);
}

TEST(IsinChangeTest, RowsWithoutAnActionIdMoreThanTenDaysApartAreNotOneChange) {
    // The in row is 11 days after the out row: no partner, so the out row becomes a merger.
    CorporateAction out = usualOutAction();
    CorporateAction in = usualInAction();

    out.mBrokerActionId = std::nullopt;
    in.mBrokerActionId = std::nullopt;
    in.mMetadata.mTaxDate = day("2024-06-14");

    StatementMergeResult merged = usualStatement(std::move(out), std::move(in));
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_TRUE(latest.mStatement.has_value());
    EXPECT_EQ(
        latest.mStatement->mPresentation.mTradeInstruments.at(0).mCorporateActions.at(0).mType,
        CorporateActionType::Merger);
}

TEST(IsinChangeTest, ARowWithAnActionIdIsNotMatchedToARowWithoutOne) {
    // The out row has an ID, the in row has none and is on the same day: still no partner.
    CorporateAction in = usualInAction();

    in.mBrokerActionId = std::nullopt;

    StatementMergeResult merged = usualStatement(usualOutAction(), std::move(in));
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_TRUE(latest.mStatement.has_value());
    EXPECT_EQ(
        latest.mStatement->mPresentation.mTradeInstruments.at(0).mCorporateActions.at(0).mType,
        CorporateActionType::Merger);
}

TEST(IsinChangeTest, UnitsThatDoNotFitGiveNoSuggestedRatio) {
    // Two out rows of 50 billion units each (5e18 in the 8 decimal units) add up to more than the
    // 64-bit maximum of about 9.2e18: the ratio is not suggested, so the answer is not compared.
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());
    auto& out = merged.mStatement.mPresentation.mTradeInstruments.at(0).mCorporateActions;

    out = {
        {.mMetadata = {.mTaxDate = day("2024-06-03"), .mSources = {source(3, "out-a")}},
         .mType = CorporateActionType::IsinChange,
         .mUnitsDelta = -5'000'000'000'000'000'000, // 50 billion units out
         .mBrokerActionId = "change-1",
         .mNewIsin = kNewIsin,
         .mUnitEvidence = sharesEvidence(source(3, "out-a"), "-50000000000")},
        {.mMetadata = {.mTaxDate = day("2024-06-03"), .mSources = {source(4, "out-b")}},
         .mType = CorporateActionType::IsinChange,
         .mUnitsDelta = -5'000'000'000'000'000'000, // 50 billion units out
         .mBrokerActionId = "change-1",
         .mNewIsin = kNewIsin,
         .mUnitEvidence = sharesEvidence(source(4, "out-b"), "-50000000000")},
    };

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data = std::get<IsinChangeQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_FALSE(data.mSuggestedRatio.has_value());
}

TEST(IsinChangeTest, RowsWithoutUnitsGiveNoSuggestedRatio) {
    // The row of the old ISIN has no units (0): nothing left, so no ratio can be shown.
    CorporateAction out = usualOutAction();

    out.mUnitsDelta = 0;
    out.mUnitEvidence = sharesEvidence(source(3, "out-1"), "0");

    StatementMergeResult merged = usualStatement(std::move(out), usualInAction());
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data = std::get<IsinChangeQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_FALSE(data.mSuggestedRatio.has_value());
}

TEST(IsinChangeTest, UnitsThatLeftAsTheSmallestNumberGiveNoSuggestedRatio) {
    // The smallest 64-bit number has no positive counterpart, so it is not a number of units that
    // left.
    CorporateAction out = usualOutAction();

    out.mUnitsDelta = std::numeric_limits<Units>::min();

    StatementMergeResult merged = usualStatement(std::move(out), usualInAction());
    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const auto& data = std::get<IsinChangeQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_FALSE(data.mSuggestedRatio.has_value());
}

TEST(IsinChangeTest, TwoChangesInARowAreAskedInDateOrderAndBothCarriedOut) {
    // A to B on 2024-06-03 (10 units, 1 for 1) and B to C on 2024-07-01 (6 units, 1 for 1). The
    // units are bought under A, 4 are sold under B on 2024-06-20 and 6 under C on 2024-09-02.
    // Both changes have a sale of one of their ISINs, so both are asked.
    StatementMergeResult merged;

    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Third Share",
            .mIsin = kThirdIsin,
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-09-02"),
                                   .mSources = {source(9, "sell-c")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'500'000'000, // EUR 15
                     .mUnits = 600'000'000,       // 6 units
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-07-01"), .mSources = {source(7, "in-bc")}},
                     .mType = CorporateActionType::IsinChange,
                     .mUnitsDelta = 600'000'000, // 6 units in
                     .mBrokerActionId = "change-bc",
                     .mUnitEvidence = sharesEvidence(source(7, "in-bc"), "6")},
                },
        },
        {
            .mName = "Synthetic New Share",
            .mIsin = kNewIsin,
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-06-20"),
                                   .mSources = {source(5, "sell-b")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'400'000'000, // EUR 14
                     .mUnits = 400'000'000,       // 4 units
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-06-03"), .mSources = {source(4, "in-ab")}},
                     .mType = CorporateActionType::IsinChange,
                     .mUnitsDelta = 1'000'000'000, // 10 units in
                     .mBrokerActionId = "change-ab",
                     .mUnitEvidence = sharesEvidence(source(4, "in-ab"), "10")},
                    {.mMetadata = {.mTaxDate = day("2024-07-01"),
                                   .mSources = {source(6, "out-bc")}},
                     .mType = CorporateActionType::IsinChange,
                     .mUnitsDelta = -600'000'000, // 6 units out
                     .mBrokerActionId = "change-bc",
                     .mNewIsin = kThirdIsin,
                     .mUnitEvidence = sharesEvidence(source(6, "out-bc"), "-6")},
                },
        },
        {
            .mName = "Synthetic Old Share",
            .mIsin = kOldIsin,
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-05-02"), .mSources = {source(2, "buy-a")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-06-03"),
                                   .mSources = {source(3, "out-ab")}},
                     .mType = CorporateActionType::IsinChange,
                     .mUnitsDelta = -1'000'000'000, // 10 units out
                     .mBrokerActionId = "change-ab",
                     .mNewIsin = kNewIsin,
                     .mUnitEvidence = sharesEvidence(source(3, "out-ab"), "-10")},
                },
        },
    };

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    // The earlier change is request-0001, whatever the order of the instruments.
    ASSERT_EQ(latest.mRequests.size(), 2U);
    EXPECT_EQ(std::get<IsinChangeQuestion>(latest.mRequests.at(0).mQuestion.mData).mOldIsin,
              kOldIsin);
    EXPECT_EQ(std::get<IsinChangeQuestion>(latest.mRequests.at(1).mQuestion.mData).mOldIsin,
              kNewIsin);

    const auto rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 1,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-06-03")}},
                        {"request-0002",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 1,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-07-01")}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(latest.mRequests.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const auto& instruments = latest.mStatement->mPresentation.mTradeInstruments;

    ASSERT_EQ(instruments.size(), 1U);
    EXPECT_EQ(instruments.at(0).mIsin, kThirdIsin);
    EXPECT_TRUE(instruments.at(0).mCorporateActions.empty());
    ASSERT_EQ(instruments.at(0).mTransactions.size(), 3U);
    // Each trade stays listed under the ISIN and name it had on its date.
    EXPECT_EQ(instruments.at(0).mTransactions.at(0).mMetadata.mTaxDate, day("2023-05-02"));
    EXPECT_EQ(instruments.at(0).mTransactions.at(0).mListedAs,
              (ListedInstrument{.mIsin = kOldIsin, .mName = "Synthetic Old Share"}));
    EXPECT_EQ(instruments.at(0).mTransactions.at(1).mMetadata.mTaxDate, day("2024-06-20"));
    EXPECT_EQ(instruments.at(0).mTransactions.at(1).mListedAs,
              (ListedInstrument{.mIsin = kNewIsin, .mName = "Synthetic New Share"}));
    EXPECT_EQ(instruments.at(0).mTransactions.at(2).mMetadata.mTaxDate, day("2024-09-02"));
    EXPECT_FALSE(instruments.at(0).mTransactions.at(2).mListedAs.has_value());

    ASSERT_EQ(latest.mDiagnostics.size(), 2U);
    EXPECT_EQ(latest.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAppliedIsinChange);
    EXPECT_EQ(latest.mDiagnostics.at(1).mMessageKey, MessageKey::CorporateActionAppliedIsinChange);
}

TEST(IsinChangeTest, AnEarlierChangeIsAskedWhenALaterChangeOfTheYearTakesTheNewIsinOut) {
    // A to B on 2023-09-01 (10 units) and B to C on 2024-07-01 (10 units). Only C is sold in
    // 2024, on 2024-09-02. B to C matters by that sale; A to B matters because the units of B
    // leave in a change of the year, which is a sale of B once it is answered.
    StatementMergeResult merged;

    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Old Share",
            .mIsin = kOldIsin,
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-05-02"), .mSources = {source(2, "buy-a")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-09-01"),
                                   .mSources = {source(3, "out-ab")}},
                     .mType = CorporateActionType::IsinChange,
                     .mUnitsDelta = -1'000'000'000, // 10 units out
                     .mBrokerActionId = "change-ab",
                     .mNewIsin = kNewIsin,
                     .mUnitEvidence = sharesEvidence(source(3, "out-ab"), "-10")},
                },
        },
        {
            .mName = "Synthetic New Share",
            .mIsin = kNewIsin,
            .mAssetClass = AssetClass::Stock,
            .mCorporateActions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-09-01"), .mSources = {source(4, "in-ab")}},
                     .mType = CorporateActionType::IsinChange,
                     .mUnitsDelta = 1'000'000'000, // 10 units in
                     .mBrokerActionId = "change-ab",
                     .mUnitEvidence = sharesEvidence(source(4, "in-ab"), "10")},
                    {.mMetadata = {.mTaxDate = day("2024-07-01"),
                                   .mSources = {source(6, "out-bc")}},
                     .mType = CorporateActionType::IsinChange,
                     .mUnitsDelta = -1'000'000'000, // 10 units out
                     .mBrokerActionId = "change-bc",
                     .mNewIsin = kThirdIsin,
                     .mUnitEvidence = sharesEvidence(source(6, "out-bc"), "-10")},
                },
        },
        {
            .mName = "Synthetic Third Share",
            .mIsin = kThirdIsin,
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-09-02"),
                                   .mSources = {source(9, "sell-c")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'500'000'000, // EUR 15
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-07-01"), .mSources = {source(7, "in-bc")}},
                     .mType = CorporateActionType::IsinChange,
                     .mUnitsDelta = 1'000'000'000, // 10 units in
                     .mBrokerActionId = "change-bc",
                     .mUnitEvidence = sharesEvidence(source(7, "in-bc"), "10")},
                },
        },
    };

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    // The earlier change is asked first, then the later one.
    ASSERT_EQ(latest.mRequests.size(), 2U);
    EXPECT_EQ(std::get<IsinChangeQuestion>(latest.mRequests.at(0).mQuestion.mData).mOldIsin,
              kOldIsin);
    EXPECT_EQ(std::get<IsinChangeQuestion>(latest.mRequests.at(1).mQuestion.mData).mOldIsin,
              kNewIsin);
}

TEST(IsinChangeTest, TheNewInstrumentTakesTheAssetClassAndNameOfTheOldOneWhenItHasNone) {
    StatementMergeResult merged = usualStatement(usualOutAction(), usualInAction());
    auto& instruments = merged.mStatement.mPresentation.mTradeInstruments;

    instruments.at(1).mName.clear();
    instruments.at(1).mAssetClass = AssetClass::Unknown;

    IsinChangeResult latest;
    DecisionSession session;

    session.start(changeCalculation(merged, 2024, latest));

    const auto rejections = session.answer({{"request-0001", 1, kSameIssuerTwoForOne}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const auto& carried = latest.mStatement->mPresentation.mTradeInstruments;

    ASSERT_EQ(carried.size(), 1U);
    EXPECT_EQ(carried.at(0).mName, "Synthetic Old Share");
    EXPECT_EQ(carried.at(0).mAssetClass, AssetClass::Stock);
}

} // namespace
