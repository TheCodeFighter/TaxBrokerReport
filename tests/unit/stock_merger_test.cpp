#include "processors/statement_merger.hpp"
#include "processors/stock_merger.hpp"
#include "processors/tax_processor.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
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
        .mBroker = Broker::InteractiveBrokers,
        .mFilename = SourceFilename::fromPath("history.xml"),
        .mSourceRow = aRow,
        .mTransactionId = std::move(aTransactionId),
    };
}

/// One calculation of a session: prepares the year from the statement and applies the stock
/// mergers with the session's answers. aLatest keeps the result of the latest calculation.
RunCalculation
mergerCalculation(StatementMergeResult& aMerged, int aYear, StockMergerResult& aLatest) {
    rebuildChronologicalOrder(aMerged.mStatement);

    return [&aMerged, aYear, &aLatest](RunDecisions& aDecisions) {
        const auto prepared = TaxProcessor::prepareReportingYear({aYear, aMerged});

        aLatest = applyStockMergers(aMerged.mStatement, prepared.mLedger.value(), aDecisions);

        ApplicationResultInput input{.mReportingYear = aYear};
        auto diagnostics = aLatest.mDiagnostics;
        auto requests = aLatest.mRequests;

        appendStageOutput(input, std::move(diagnostics), std::move(requests));

        return input;
    };
}

/// A row of a merger: aUnits is negative for the old ISIN, positive for the new one.
CorporateAction row(const std::string& aTransactionId,
                    std::size_t aSourceRow,
                    std::string_view aDate,
                    Units aUnits,
                    std::optional<std::string> aActionId) {
    return {.mMetadata = {.mTaxDate = day(aDate), .mSources = {source(aSourceRow, aTransactionId)}},
            .mType = CorporateActionType::StockMerger,
            .mUnitsDelta = aUnits,
            .mBrokerActionId = std::move(aActionId),
            .mAmountCurrency = Currency::EUR};
}

/// The statement of the usual case: 10 units of the old ISIN bought on 2023-05-02 at EUR 10, and
/// a merger on 2024-06-03 whose two rows are given by the test: aOutAction and aInAction.
StatementMergeResult mergerStatement(CorporateAction aOutAction, CorporateAction aInAction) {
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
            .mCorporateActions = {std::move(aInAction)},
        },
    };

    return merged;
}

const AnswerValue kTaxableEur100 = StockMergerAnswer{.mTreatment = StockMergerTreatment::Taxable,
                                                     .mMarketValue = 1'000'000, // EUR 100
                                                     .mEffectiveDate = day("2024-06-03")};

TEST(StockMergerTest, AStatementWithoutAStockMergerIsNotChanged) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Old Share",
        .mIsin = kOldIsin,
        .mAssetClass = AssetClass::Stock,
        .mCorporateActions = {{.mMetadata = {.mTaxDate = day("2024-06-03"),
                                             .mSources = {source(3, "split-1")}},
                               .mType = CorporateActionType::UnresolvedSplit,
                               .mUnitsDelta = 1'000'000'000}},
    }};
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_FALSE(latest.mStatement.has_value());
    EXPECT_TRUE(latest.mDiagnostics.empty());
    EXPECT_TRUE(latest.mRequests.empty());
}

TEST(StockMergerTest, RowsWithoutAnActionIdOfTheSameWeekAreOneMerger) {
    // 2024-06-03 and 2024-06-09 are 6 days apart, within the 10 days of one action.
    StatementMergeResult merged =
        mergerStatement(row("out-1", 3, "2024-06-03", -1'000'000'000, std::nullopt),
                        row("in-1", 4, "2024-06-09", 400'000'000, std::nullopt));
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const StockMergerQuestion& question =
        std::get<StockMergerQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(question.mOldIsin, kOldIsin);
    EXPECT_EQ(question.mNewIsin, kNewIsin);
    EXPECT_EQ(question.mEffectiveDate, day("2024-06-03"));
    EXPECT_EQ(question.mEarliestDate, day("2024-06-03"));
    EXPECT_EQ(question.mLatestDate, day("2024-06-09"));
}

TEST(StockMergerTest, RowsWithoutAnActionIdMoreThanTenDaysApartAreNotOneMerger) {
    // 2024-06-03 and 2024-06-14 are 11 days apart: two groups of one row each, so neither has
    // both an old and a new ISIN. Nothing is asked and the rows stay as they are.
    StatementMergeResult merged =
        mergerStatement(row("out-1", 3, "2024-06-03", -1'000'000'000, std::nullopt),
                        row("in-1", 4, "2024-06-14", 400'000'000, std::nullopt));
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    EXPECT_TRUE(latest.mDiagnostics.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
}

TEST(StockMergerTest, ARowWithAnIdAndARowWithoutOneAreNotOneMerger) {
    StatementMergeResult merged =
        mergerStatement(row("out-1", 3, "2024-06-03", -1'000'000'000, "merger-1"),
                        row("in-1", 4, "2024-06-03", 400'000'000, std::nullopt));
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
}

TEST(StockMergerTest, ARowWithoutUnitsIsNotAMergerOfTwoCompanies) {
    StatementMergeResult merged =
        mergerStatement(row("out-1", 3, "2024-06-03", -1'000'000'000, "merger-1"),
                        row("in-1", 4, "2024-06-03", 0, "merger-1"));
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
}

TEST(StockMergerTest, UnitsLeavingAndComingInOnOneIsinAreNotAMergerOfTwoCompanies) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Old Share",
        .mIsin = kOldIsin,
        .mAssetClass = AssetClass::Stock,
        .mCorporateActions = {row("out-1", 3, "2024-06-03", -1'000'000'000, "merger-1"),
                              row("in-1", 4, "2024-06-03", 400'000'000, "merger-1")},
    }};
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
}

TEST(StockMergerTest, RowsWithoutACurrencyAreNotAsked) {
    CorporateAction out = row("out-1", 3, "2024-06-03", -1'000'000'000, "merger-1");
    CorporateAction in = row("in-1", 4, "2024-06-03", 400'000'000, "merger-1");

    out.mAmountCurrency = Currency::Unknown;

    StatementMergeResult merged = mergerStatement(std::move(out), std::move(in));
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
}

TEST(StockMergerTest, RowsInTwoCurrenciesAreNotAsked) {
    CorporateAction out = row("out-1", 3, "2024-06-03", -1'000'000'000, "merger-1");
    CorporateAction in = row("in-1", 4, "2024-06-03", 400'000'000, "merger-1");

    in.mAmountCurrency = Currency::USD;

    StatementMergeResult merged = mergerStatement(std::move(out), std::move(in));
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
}

TEST(StockMergerTest, CashThatDoesNotFitLeavesTheMergerUnsupported) {
    // Two rows of the old ISIN with the largest cash each: the total does not fit 64 bits.
    CorporateAction out1 = row("out-1", 3, "2024-06-03", -500'000'000, "merger-1");
    CorporateAction out2 = row("out-2", 5, "2024-06-03", -500'000'000, "merger-1");

    out1.mCashProceeds = std::numeric_limits<Money>::max();
    out2.mCashProceeds = std::numeric_limits<Money>::max();

    StatementMergeResult merged =
        mergerStatement(std::move(out1), row("in-1", 4, "2024-06-03", 400'000'000, "merger-1"));

    merged.mStatement.mPresentation.mTradeInstruments.at(0).mCorporateActions.push_back(
        std::move(out2));

    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
}

TEST(StockMergerTest, AValueOnOnlyOneOfTwoRowsOfTheNewIsinIsNotSuggested) {
    // Two rows of the new ISIN (two accounts): one gives the value, one does not, so the total is
    // not known and nothing is suggested.
    CorporateAction in1 = row("in-1", 4, "2024-06-03", 200'000'000, "merger-1");
    CorporateAction in2 = row("in-2", 5, "2024-06-03", 200'000'000, "merger-1");

    in1.mMarketValue = 2'600'000; // EUR 260

    StatementMergeResult merged =
        mergerStatement(row("out-1", 3, "2024-06-03", -1'000'000'000, "merger-1"), std::move(in1));

    merged.mStatement.mPresentation.mTradeInstruments.at(1).mCorporateActions.push_back(
        std::move(in2));

    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const StockMergerQuestion& question =
        std::get<StockMergerQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_FALSE(question.mSuggestedValue.has_value());
    EXPECT_EQ(question.mNewUnits, 400'000'000); // 4 units
}

TEST(StockMergerTest, ValuesOnBothRowsOfTheNewIsinAreAddedUp) {
    CorporateAction in1 = row("in-1", 4, "2024-06-03", 200'000'000, "merger-1");
    CorporateAction in2 = row("in-2", 5, "2024-06-03", 200'000'000, "merger-1");

    in1.mMarketValue = 2'600'000; // EUR 260
    in2.mMarketValue = 2'600'000; // EUR 260

    StatementMergeResult merged =
        mergerStatement(row("out-1", 3, "2024-06-03", -1'000'000'000, "merger-1"), std::move(in1));

    merged.mStatement.mPresentation.mTradeInstruments.at(1).mCorporateActions.push_back(
        std::move(in2));

    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);
    EXPECT_EQ(std::get<StockMergerQuestion>(latest.mRequests.at(0).mQuestion.mData).mSuggestedValue,
              std::optional<Money>{5'200'000}); // EUR 520
}

TEST(StockMergerTest, AnOpenQuestionMarksBothRowsAsPending) {
    StatementMergeResult merged =
        mergerStatement(row("out-1", 3, "2024-06-03", -1'000'000'000, "merger-1"),
                        row("in-1", 4, "2024-06-03", 400'000'000, "merger-1"));
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    ASSERT_TRUE(latest.mStatement.has_value());

    const std::vector<TradeInstrument>& instruments =
        latest.mStatement->mPresentation.mTradeInstruments;

    EXPECT_EQ(instruments.at(0).mCorporateActions.at(0).mType,
              CorporateActionType::StockMergerPending);
    EXPECT_EQ(instruments.at(1).mCorporateActions.at(0).mType,
              CorporateActionType::StockMergerPending);
}

TEST(StockMergerTest, ATaxableAnswerReplacesTheRowsWithASaleAndAPurchase) {
    // 10 units of the old ISIN become 4 units of the new one; value EUR 100: a sale of 10 units at
    // EUR 10 and a purchase of 4 units at EUR 25, both on 2024-06-03, the purchase marked `E`.
    StatementMergeResult merged =
        mergerStatement(row("out-1", 3, "2024-06-03", -1'000'000'000, "merger-1"),
                        row("in-1", 4, "2024-06-03", 400'000'000, "merger-1"));
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(session.answer({{"request-0001", 1, kTaxableEur100}}).empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const std::vector<TradeInstrument>& instruments =
        latest.mStatement->mPresentation.mTradeInstruments;

    EXPECT_TRUE(instruments.at(0).mCorporateActions.empty());
    EXPECT_TRUE(instruments.at(1).mCorporateActions.empty());
    ASSERT_EQ(instruments.at(0).mTransactions.size(), 2U);
    ASSERT_EQ(instruments.at(1).mTransactions.size(), 1U);

    const TradeTransaction& sale = instruments.at(0).mTransactions.at(1);
    const TradeTransaction& purchase = instruments.at(1).mTransactions.at(0);

    EXPECT_EQ(sale.mTradeSide, TradeSide::Sell);
    EXPECT_TRUE(sale.mFromStockMerger);
    EXPECT_FALSE(sale.mFromCashMerger);
    EXPECT_EQ(sale.mMetadata.mTaxDate, day("2024-06-03"));
    EXPECT_EQ(sale.mUnits, 1'000'000'000);     // 10 units
    EXPECT_EQ(sale.mUnitPrice, 1'000'000'000); // EUR 100 / 10 units = EUR 10
    EXPECT_EQ(sale.mCurrency, Currency::EUR);
    EXPECT_EQ(sale.mMetadata.mSources, (std::vector<SourceReference>{source(3, "out-1")}));

    EXPECT_EQ(purchase.mTradeSide, TradeSide::Buy);
    EXPECT_FALSE(purchase.mFromStockMerger);
    EXPECT_EQ(purchase.mPurchaseKind, PurchaseKind::StatusChangeExchange);
    EXPECT_EQ(purchase.mMetadata.mTaxDate, day("2024-06-03"));
    EXPECT_EQ(purchase.mUnits, 400'000'000);       // 4 units
    EXPECT_EQ(purchase.mUnitPrice, 2'500'000'000); // EUR 100 / 4 units = EUR 25
    EXPECT_EQ(purchase.mMetadata.mSources, (std::vector<SourceReference>{source(4, "in-1")}));
}

TEST(StockMergerTest, AnAnswerDateOtherThanTheRowsHasNoTimeOfDay) {
    CorporateAction out = row("out-1", 3, "2024-06-03", -1'000'000'000, "merger-1");

    out.mMetadata.mSourceTimestamp = SourceTimestamp{std::chrono::milliseconds{1'717'400'000'000}};

    StatementMergeResult merged =
        mergerStatement(std::move(out), row("in-1", 4, "2024-06-05", 400'000'000, "merger-1"));
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(session
                    .answer({{"request-0001",
                              1,
                              StockMergerAnswer{.mTreatment = StockMergerTreatment::Taxable,
                                                .mMarketValue = 1'000'000,
                                                .mEffectiveDate = day("2024-06-04")}}})
                    .empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const TradeTransaction& sale =
        latest.mStatement->mPresentation.mTradeInstruments.at(0).mTransactions.at(1);

    EXPECT_EQ(sale.mMetadata.mTaxDate, day("2024-06-04"));
    EXPECT_FALSE(sale.mMetadata.mSourceTimestamp.has_value());
    EXPECT_FALSE(sale.mMetadata.mOrderingTimestamp.has_value());
}

TEST(StockMergerTest, AValueTooSmallForThePriceLeavesTheRowsAndAddsNoWarning) {
    // EUR 0.0001 over 1,000,000 units is EUR 0.0000000001 each, which is 0 at 8 decimals.
    StatementMergeResult merged =
        mergerStatement(row("out-1", 3, "2024-06-03", -100'000'000'000'000, "merger-1"),
                        row("in-1", 4, "2024-06-03", 400'000'000, "merger-1"));
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(session
                    .answer({{"request-0001",
                              1,
                              StockMergerAnswer{.mTreatment = StockMergerTreatment::Taxable,
                                                .mMarketValue = 1, // EUR 0.0001
                                                .mEffectiveDate = day("2024-06-03")}}})
                    .empty());

    EXPECT_TRUE(latest.mDiagnostics.empty());
    ASSERT_TRUE(latest.mStatement.has_value());
    EXPECT_EQ(
        latest.mStatement->mPresentation.mTradeInstruments.at(0).mCorporateActions.at(0).mType,
        CorporateActionType::StockMerger);
}

TEST(StockMergerTest, UnitsThatDoNotAddUpLeaveTheMergerUnsupported) {
    // Two rows of the old ISIN with 5,000,000,000 units each: the total does not fit 64 bits.
    CorporateAction out1 = row("out-1", 3, "2024-06-03", -5'000'000'000'000'000'000, "merger-1");
    CorporateAction out2 = row("out-2", 5, "2024-06-03", -5'000'000'000'000'000'000, "merger-1");
    StatementMergeResult merged =
        mergerStatement(std::move(out1), row("in-1", 4, "2024-06-03", 400'000'000, "merger-1"));

    merged.mStatement.mPresentation.mTradeInstruments.at(0).mCorporateActions.push_back(
        std::move(out2));

    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
}

TEST(StockMergerTest, ADeferredAnswerCarriesTheLotsWithTheReducedRatioOfTheUnits) {
    // 10 units become 4: the ratio is 4 / 10 = 2 / 5, and the carried purchase is marked `E`.
    StatementMergeResult merged =
        mergerStatement(row("out-1", 3, "2024-06-03", -1'000'000'000, "merger-1"),
                        row("in-1", 4, "2024-06-03", 400'000'000, "merger-1"));
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(session
                    .answer({{"request-0001",
                              1,
                              StockMergerAnswer{.mTreatment = StockMergerTreatment::Deferred,
                                                .mMarketValue = 0,
                                                .mEffectiveDate = day("2024-06-03")}}})
                    .empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const std::vector<TradeInstrument>& instruments =
        latest.mStatement->mPresentation.mTradeInstruments;

    // The old instrument is gone; its purchase is under the new ISIN, marked `E`.
    ASSERT_EQ(instruments.size(), 1U);
    EXPECT_EQ(instruments.at(0).mIsin, kNewIsin);
    ASSERT_EQ(instruments.at(0).mTransactions.size(), 1U);
    EXPECT_EQ(instruments.at(0).mTransactions.at(0).mPurchaseKind,
              PurchaseKind::StatusChangeExchange);

    ASSERT_EQ(instruments.at(0).mCorporateActions.size(), 1U);
    EXPECT_EQ(instruments.at(0).mCorporateActions.at(0).mType, CorporateActionType::ReverseSplit);
    EXPECT_EQ(instruments.at(0).mCorporateActions.at(0).mRatio,
              (SplitRatio{.mNewShares = 2, .mOldShares = 5}));
}

TEST(StockMergerTest, ATaxableAnswerAfterAnEarlierMergerFindsTheLaterOneAgain) {
    // A to B on 2024-03-01 (value EUR 100) and B to C on 2024-06-03 (value EUR 200). Both are
    // asked in one run; both answers are applied, the second after the first changed the rows.
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
            .mCorporateActions = {row("out-1", 3, "2024-03-01", -1'000'000'000, "merger-1")},
        },
        {
            .mName = "Synthetic New Share",
            .mIsin = kNewIsin,
            .mAssetClass = AssetClass::Stock,
            .mCorporateActions = {row("in-1", 4, "2024-03-01", 400'000'000, "merger-1"),
                                  row("out-2", 5, "2024-06-03", -400'000'000, "merger-2")},
        },
        {
            .mName = "Synthetic Third Share",
            .mIsin = kThirdIsin,
            .mAssetClass = AssetClass::Stock,
            .mCorporateActions = {row("in-2", 6, "2024-06-03", 100'000'000, "merger-2")},
        },
    };

    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 2U);

    // The requests are in the order of the mergers.
    const std::vector<DecisionRequest>& asked = session.result().mRequests;

    ASSERT_EQ(asked.size(), 2U);
    EXPECT_EQ(asked.at(0).mIsin, kOldIsin);
    EXPECT_EQ(asked.at(1).mIsin, kNewIsin);

    const std::vector<RejectedAnswer> rejections =
        session.answer({{asked.at(0).mId,
                         1,
                         StockMergerAnswer{.mTreatment = StockMergerTreatment::Taxable,
                                           .mMarketValue = 1'000'000, // EUR 100
                                           .mEffectiveDate = day("2024-03-01")}},
                        {asked.at(1).mId,
                         1,
                         StockMergerAnswer{.mTreatment = StockMergerTreatment::Taxable,
                                           .mMarketValue = 2'000'000, // EUR 200
                                           .mEffectiveDate = day("2024-06-03")}}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_TRUE(latest.mStatement.has_value());

    const std::vector<TradeInstrument>& instruments =
        latest.mStatement->mPresentation.mTradeInstruments;

    // Old share: its purchase and the sale of 10 units. New share: the purchase of 4 units on
    // 2024-03-01 and the sale of 4 units on 2024-06-03. Third share: the purchase of 1 unit.
    ASSERT_EQ(instruments.at(0).mTransactions.size(), 2U);
    ASSERT_EQ(instruments.at(1).mTransactions.size(), 2U);
    ASSERT_EQ(instruments.at(2).mTransactions.size(), 1U);
    EXPECT_TRUE(instruments.at(1).mCorporateActions.empty());
    EXPECT_EQ(instruments.at(1).mTransactions.at(0).mTradeSide, TradeSide::Buy);
    EXPECT_EQ(instruments.at(1).mTransactions.at(0).mUnitPrice, 2'500'000'000); // EUR 100 / 4
    EXPECT_EQ(instruments.at(1).mTransactions.at(1).mTradeSide, TradeSide::Sell);
    EXPECT_EQ(instruments.at(1).mTransactions.at(1).mUnitPrice, 5'000'000'000);  // EUR 200 / 4
    EXPECT_EQ(instruments.at(2).mTransactions.at(0).mUnitPrice, 20'000'000'000); // EUR 200 / 1
}

TEST(StockMergerTest, AnEarlierMergerIsAskedWhenALaterMergerOfTheYearTakesTheNewSharesOut) {
    // A to B on 2023-09-01 and B to C on 2024-06-03; nothing is traded in 2024. The 2024 merger
    // is in the year; the units of B leave in it, which is a sale of B once it is answered, so
    // the 2023 merger is asked too.
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
            .mCorporateActions = {row("out-1", 3, "2023-09-01", -1'000'000'000, "merger-1")},
        },
        {
            .mName = "Synthetic New Share",
            .mIsin = kNewIsin,
            .mAssetClass = AssetClass::Stock,
            .mCorporateActions = {row("in-1", 4, "2023-09-01", 400'000'000, "merger-1"),
                                  row("out-2", 5, "2024-06-03", -400'000'000, "merger-2")},
        },
        {
            .mName = "Synthetic Third Share",
            .mIsin = kThirdIsin,
            .mAssetClass = AssetClass::Stock,
            .mCorporateActions = {row("in-2", 6, "2024-06-03", 100'000'000, "merger-2")},
        },
    };

    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    ASSERT_EQ(latest.mRequests.size(), 2U);
    EXPECT_EQ(latest.mRequests.at(0).mQuestion.mIsin, kOldIsin);
    EXPECT_EQ(latest.mRequests.at(1).mQuestion.mIsin, kNewIsin);
}

TEST(StockMergerTest, AnEarlierMergerIsNotAskedWhenALaterMergerIsAfterTheYear) {
    // The same two mergers, the second on 2025-06-03: it is after 2024 and after the loss view
    // of 2024 (2025-01-30), and nothing is traded in 2024, so neither merger matters.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Old Share",
            .mIsin = kOldIsin,
            .mAssetClass = AssetClass::Stock,
            .mCorporateActions = {row("out-1", 3, "2023-09-01", -1'000'000'000, "merger-1")},
        },
        {
            .mName = "Synthetic New Share",
            .mIsin = kNewIsin,
            .mAssetClass = AssetClass::Stock,
            .mCorporateActions = {row("in-1", 4, "2023-09-01", 400'000'000, "merger-1"),
                                  row("out-2", 5, "2025-06-03", -400'000'000, "merger-2")},
        },
        {
            .mName = "Synthetic Third Share",
            .mIsin = kThirdIsin,
            .mAssetClass = AssetClass::Stock,
            .mCorporateActions = {row("in-2", 6, "2025-06-03", 100'000'000, "merger-2")},
        },
    };

    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2024, latest));

    EXPECT_TRUE(latest.mRequests.empty());
    EXPECT_FALSE(latest.mStatement.has_value());
}

TEST(StockMergerTest, RowsAcrossTheNewYearWithoutAnActionIdAreAskedInTheYearOfTheLastRow) {
    // The rows (no action ID) are on 2024-12-30 and 2025-01-02, 3 days apart, so one merger. A
    // row is in 2025 and nothing is traded, so it is asked in the 2025 run too.
    StatementMergeResult merged =
        mergerStatement(row("out-1", 3, "2024-12-30", -1'000'000'000, std::nullopt),
                        row("in-1", 4, "2025-01-02", 400'000'000, std::nullopt));
    StockMergerResult latest;
    DecisionSession session;

    session.start(mergerCalculation(merged, 2025, latest));

    ASSERT_EQ(latest.mRequests.size(), 1U);

    const StockMergerQuestion& asked =
        std::get<StockMergerQuestion>(latest.mRequests.at(0).mQuestion.mData);

    EXPECT_EQ(asked.mEarliestDate, day("2024-12-30"));
    EXPECT_EQ(asked.mLatestDate, day("2025-01-02"));
}

} // namespace
