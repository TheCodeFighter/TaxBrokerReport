#include "processors/tax_classifier.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

constexpr Units kOneUnit = 100'000'000; // Units have eight decimals

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

/// The evidence a row leaves about the class of its instrument.
InstrumentSourceEvidence evidence(std::size_t aRow, AssetClass aClass, const std::string& aLabel) {
    return InstrumentSourceEvidence{
        .mSource = source(aRow, "synthetic-" + std::to_string(aRow)),
        .mName = "Synthetic Instrument",
        .mAssetClass = aClass,
        .mAssetClassLabel = aLabel,
    };
}

/// A prepared ledger for the year, with no events: the classifier reads only its year bounds.
ReportingYearLedger ledgerOf(int aYear) {
    const auto start = day(std::to_string(aYear) + "-01-01");
    const auto end = day(std::to_string(aYear) + "-12-31");

    return ReportingYearLedger{.mYear = aYear, .mYearStart = start, .mYearEnd = end};
}

/// Runs the classifier in a session, so a test can answer its questions.
struct ClassifierRun {
    DecisionSession mSession;
    MergedStatement mStatement;
    std::vector<StatementMergeDiagnostic> mMergeDiagnostics;
    ReportingYearLedger mLedger;
    TaxClassificationResult mLast;

    ClassifierRun(MergedStatement aStatement,
                  std::vector<StatementMergeDiagnostic> aMergeDiagnostics,
                  int aYear)
        : mStatement(std::move(aStatement)), mMergeDiagnostics(std::move(aMergeDiagnostics)),
          mLedger(ledgerOf(aYear)) {
        mSession.start([this](RunDecisions& aDecisions) {
            mLast = TaxClassifier::classify(mStatement, mLedger, mMergeDiagnostics, aDecisions);

            ApplicationResultInput input{.mReportingYear = mLedger.mYear,
                                         .mMergeDiagnostics = mMergeDiagnostics};
            auto copy = mLast;

            appendClassification(input, std::move(copy));

            return input;
        });
    }

    [[nodiscard]] const ApplicationResult& result() const {
        return mSession.result();
    }

    /// Answers the first open request.
    void answerFirst(AnswerValue aAnswer) {
        const auto rejections = mSession.answer({DecisionAnswer{
            .mRequestId = result().mRequests.at(0).mId,
            .mRunRevision = result().mRunRevision,
            .mValue = std::move(aAnswer),
        }});

        ASSERT_TRUE(rejections.empty());
    }
};

MergedStatement unknownShare(const std::string& aLabel) {
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Unknown,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Unknown, aLabel)}},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(3, AssetClass::Unknown, aLabel)}},
            },
    }};

    return statement;
}

TEST(TaxClassifierTest, SharesAndFundUnitsNeedNoQuestionAndStayInCapitalGains) {
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {
        {.mName = "Synthetic Share",
         .mIsin = "XX0000000001",
         .mAssetClass = AssetClass::Stock,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2024-01-10"),
                                          .mSources = {source(2, "synthetic-buy-1")}},
                            .mTradeSide = TradeSide::Buy,
                            .mUnits = kOneUnit,
                            .mCurrency = Currency::EUR}}},
        {.mName = "Synthetic Fund",
         .mIsin = "XX0000000002",
         .mAssetClass = AssetClass::Fund,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2024-01-11"),
                                          .mSources = {source(3, "synthetic-buy-2")}},
                            .mTradeSide = TradeSide::Buy,
                            .mUnits = kOneUnit,
                            .mCurrency = Currency::EUR}}},
    };

    const ClassifierRun run{std::move(statement), {}, 2024};

    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
    EXPECT_TRUE(run.mLast.mRequests.empty());
    EXPECT_TRUE(run.mLast.mExcludedItems.empty());
    EXPECT_TRUE(run.mLast.mLeftOutIsins.empty());
}

TEST(TaxClassifierTest, BondTradesOfTheYearAreOneExcludedItemWithAWarning) {
    MergedStatement statement;

    // 1000 units bought in 2024 (not the selected year), 400 bought and 250 sold in 2025.
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Bond,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-12-30"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnits = 1000 * kOneUnit,
                 .mCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Bond, "BOND")}},
                {.mMetadata = {.mTaxDate = day("2025-02-03"),
                               .mSources = {source(3, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnits = 400 * kOneUnit,
                 .mCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(3, AssetClass::Bond, "BOND")}},
                {.mMetadata = {.mTaxDate = day("2025-05-02"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnits = 250 * kOneUnit,
                 .mCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(4, AssetClass::Bond, "BOND")}},
            },
    }};

    const ClassifierRun run{std::move(statement), {}, 2025};

    EXPECT_EQ(run.mLast.mLeftOutIsins, (std::set<Isin>{"XX0000000001"}));
    EXPECT_TRUE(run.mLast.mRequests.empty());

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::TaxCategoryNotReported);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mMessageKey,
              MessageKey::TaxCategoryNotReportedDebtSecurity);

    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);

    const auto& item = run.result().mExcludedItems.at(0);

    EXPECT_EQ(item.mIsin, "XX0000000001");
    EXPECT_EQ(item.mCategory, ExcludedCategory::Bond);
    EXPECT_EQ(item.mReason, ExcludedReason::DebtSecurity);
    EXPECT_TRUE(item.isTaxRule());
    EXPECT_EQ(item.mClassLabels, std::vector<std::string>{"BOND"});
    EXPECT_EQ(item.mDiagnosticIds, std::vector<std::string>{"diagnostic-0001"});
    EXPECT_EQ(item.mSources.size(), 2U); // the 2025 purchase and the 2025 sale only

    ASSERT_TRUE(item.mBond.has_value());
    EXPECT_EQ(item.mBond->mBoughtUnits, 400 * kOneUnit); // the 2024 purchase is not counted
    EXPECT_EQ(item.mBond->mSoldUnits, 250 * kOneUnit);
    EXPECT_EQ(item.mBond->mMaturedUnits, 0);
    EXPECT_FALSE(item.mBond->mMaturityDate.has_value());
}

TEST(TaxClassifierTest, BondMaturityCorrectionIsAddedUpBySumNotByOrder) {
    MergedStatement statement;

    // The rows of a corrected maturity on 2025-06-30: the cancellation is listed first because
    // its later datetime sorts it before the maturity it cancels. 1000 units leave, 1000 come
    // back, 1000 leave again: 1000 matured. The payment rows add no units. The ISIN has no trade
    // in the files.
    statement.mPresentation.mBondRedemptions = {
        {.mMetadata = {.mTaxDate = day("2025-06-30"), .mSources = {source(5, "synthetic-5")}},
         .mKind = BondRedemptionKind::MaturityCancelled,
         .mName = "Synthetic Bond",
         .mIsin = "XX0000000001",
         .mUnits = 1000 * kOneUnit},
        {.mMetadata = {.mTaxDate = day("2025-06-30"), .mSources = {source(2, "synthetic-2")}},
         .mKind = BondRedemptionKind::Maturity,
         .mName = "Synthetic Bond",
         .mIsin = "XX0000000001",
         .mUnits = 1000 * kOneUnit},
        {.mMetadata = {.mTaxDate = day("2025-06-30"), .mSources = {source(3, "synthetic-3")}},
         .mKind = BondRedemptionKind::Payment,
         .mName = "Synthetic Bond",
         .mIsin = "XX0000000001"},
        {.mMetadata = {.mTaxDate = day("2025-06-30"), .mSources = {source(6, "synthetic-6")}},
         .mKind = BondRedemptionKind::Maturity,
         .mName = "Synthetic Bond",
         .mIsin = "XX0000000001",
         .mUnits = 1000 * kOneUnit},
    };

    const ClassifierRun run{std::move(statement), {}, 2025};

    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);

    // No file gave a class for this ISIN, so there is no label.
    EXPECT_TRUE(run.result().mExcludedItems.at(0).mClassLabels.empty());

    const auto& bond = run.result().mExcludedItems.at(0).mBond;

    ASSERT_TRUE(bond.has_value());
    EXPECT_EQ(bond->mBoughtUnits, 0);
    EXPECT_EQ(bond->mSoldUnits, 0);
    EXPECT_EQ(bond->mMaturedUnits, 1000 * kOneUnit); // 1000 - 1000 + 1000
    EXPECT_EQ(bond->mMaturityDate, day("2025-06-30"));
}

TEST(TaxClassifierTest, BondRowsOfAnIsinThatTradesAsAStockAreNotBondListEntries) {
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-02-03"),
                                         .mSources = {source(2, "synthetic-buy-1")}},
                           .mTradeSide = TradeSide::Buy,
                           .mUnits = kOneUnit,
                           .mCurrency = Currency::EUR}},
    }};
    statement.mPresentation.mBondRedemptions = {
        {.mMetadata = {.mTaxDate = day("2025-06-30"), .mSources = {source(3, "synthetic-3")}},
         .mKind = BondRedemptionKind::Maturity,
         .mName = "Synthetic Share",
         .mIsin = "XX0000000001",
         .mUnits = kOneUnit},
    };

    const ClassifierRun run{std::move(statement), {}, 2025};

    EXPECT_TRUE(run.result().mExcludedItems.empty());
    EXPECT_TRUE(run.mLast.mLeftOutIsins.empty());
}

TEST(TaxClassifierTest, BondTradedOnlyInEarlierYearsIsLeftOutWithoutAnItem) {
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Bond,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2023-02-03"),
                                         .mSources = {source(2, "synthetic-buy-1")}},
                           .mTradeSide = TradeSide::Buy,
                           .mUnits = kOneUnit,
                           .mCurrency = Currency::EUR}},
    }};

    const ClassifierRun run{std::move(statement), {}, 2025};

    EXPECT_EQ(run.mLast.mLeftOutIsins, (std::set<Isin>{"XX0000000001"}));
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
    EXPECT_TRUE(run.result().mExcludedItems.empty());
}

TEST(TaxClassifierTest, BondUnitsThatDoNotFitGiveAnItemWithoutUnitFacts) {
    MergedStatement statement;

    // Two purchases of 60 billion units each add up to 1.2e19 ticks of 10^-8, above the int64
    // limit of about 9.22e18.
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Bond,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-03"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnits = 6'000'000'000'000'000'000,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-02-04"),
                               .mSources = {source(3, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnits = 6'000'000'000'000'000'000,
                 .mCurrency = Currency::EUR},
            },
    }};

    const ClassifierRun run{std::move(statement), {}, 2025};

    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);
    EXPECT_EQ(run.result().mExcludedItems.at(0).mCategory, ExcludedCategory::Bond);
    EXPECT_FALSE(run.result().mExcludedItems.at(0).mBond.has_value());
}

TEST(TaxClassifierTest, CryptoTradesOfTheYearAreOneExcludedItemWithAWarning) {
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Coin",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Crypto,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-03"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Crypto, "CRYPTO")}},
                {.mMetadata = {.mTaxDate = day("2025-05-02"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(3, AssetClass::Crypto, "CRYPTO")}},
            },
    }};

    const ClassifierRun run{std::move(statement), {}, 2025};

    EXPECT_EQ(run.mLast.mLeftOutIsins, (std::set<Isin>{"XX0000000001"}));

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mMessageKey, MessageKey::TaxCategoryNotReportedCrypto);

    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);

    const auto& item = run.result().mExcludedItems.at(0);

    EXPECT_EQ(item.mCategory, ExcludedCategory::Crypto);
    EXPECT_EQ(item.mReason, ExcludedReason::CryptoNotReported);
    EXPECT_TRUE(item.isTaxRule());
    EXPECT_EQ(item.mClassLabels, std::vector<std::string>{"CRYPTO"});
    EXPECT_FALSE(item.mBond.has_value());
    EXPECT_EQ(item.mSources.size(), 2U);
}

TEST(TaxClassifierTest, CryptoTradedOnlyInAnEarlierYearIsLeftOutWithoutAnItem) {
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Coin",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Crypto,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2024-02-03"),
                                         .mSources = {source(2, "synthetic-buy-1")}},
                           .mTradeSide = TradeSide::Buy,
                           .mUnits = kOneUnit,
                           .mCurrency = Currency::EUR}},
    }};

    const ClassifierRun run{std::move(statement), {}, 2025};

    EXPECT_EQ(run.mLast.mLeftOutIsins, (std::set<Isin>{"XX0000000001"}));
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
    EXPECT_TRUE(run.result().mExcludedItems.empty());
}

TEST(TaxClassifierTest, DerivativeAndPrivateFundAreLeftToTheCapitalGates) {
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {
        {.mName = "Synthetic Derivative",
         .mIsin = "XX0000000001",
         .mAssetClass = AssetClass::Derivative,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-02-03"),
                                          .mSources = {source(2, "synthetic-buy-1")}},
                            .mTradeSide = TradeSide::Sell,
                            .mUnits = kOneUnit,
                            .mCurrency = Currency::EUR}}},
        {.mName = "Synthetic Private Fund",
         .mIsin = "XX0000000002",
         .mAssetClass = AssetClass::PrivateFund,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-02-04"),
                                          .mSources = {source(3, "synthetic-buy-2")}},
                            .mTradeSide = TradeSide::Sell,
                            .mUnits = kOneUnit,
                            .mCurrency = Currency::EUR}}},
    };

    const ClassifierRun run{std::move(statement), {}, 2025};

    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
    EXPECT_TRUE(run.mLast.mRequests.empty());
    EXPECT_TRUE(run.mLast.mLeftOutIsins.empty());
}

TEST(TaxClassifierTest, BenefitOnlyIsinIsNotClassified) {
    // A stock perk of an ISIN that has no trade is a benefit event, not a trade instrument, even
    // though its asset class is unknown.
    MergedStatement statement;

    statement.mPresentation.mBenefitEvents = {{
        .mMetadata = {.mTaxDate = day("2024-02-01"), .mSources = {source(2, "synthetic-perk-1")}},
        .mType = BenefitType::StockPerk,
        .mName = "Synthetic Perk Share",
        .mIsin = "XX0000000002",
        .mAssetClass = AssetClass::Unknown,
    }};

    const ClassifierRun run{std::move(statement), {}, 2024};

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.result().mDiagnostics.empty());
    EXPECT_TRUE(run.result().mExcludedItems.empty());
    EXPECT_TRUE(run.mLast.mLeftOutIsins.empty());
}

TEST(TaxClassifierTest, BlankClassAsksOneQuestionAndBlocksUntilAnswered) {
    const ClassifierRun run{unknownShare(""), {}, 2024};

    // Two rows of one instrument are one question, with the blank label once.
    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    EXPECT_EQ(request.mType, RequestType::TaxClassificationRequired);
    EXPECT_EQ(request.mAffectedReports, std::vector<ReportType>{ReportType::CapitalGains});
    EXPECT_EQ(request.mIsin, "XX0000000001");
    EXPECT_EQ(request.mTaxDate, day("2024-01-10"));

    const auto* data = std::get_if<TaxClassificationQuestion>(&request.mData);

    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->mName, "Synthetic Share");
    EXPECT_EQ(data->mClassLabels, std::vector<std::string>{""});

    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);

    const auto& diagnostic = run.result().mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_TRUE(diagnostic.mBlocking);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::TaxClassificationUnknownConfirmationRequired);
    EXPECT_EQ(diagnostic.mRequestId, request.mId);
    EXPECT_EQ(run.result().report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    EXPECT_TRUE(run.mLast.mLeftOutIsins.empty());
}

TEST(TaxClassifierTest, ConflictingLabelsAreListedSortedWithoutASuggestedChoice) {
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Unknown,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
                {.mMetadata = {.mTaxDate = day("2024-02-10"),
                               .mSources = {source(3, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(3, AssetClass::Fund, "FUND")}},
            },
    }};

    const ClassifierRun run{std::move(statement), {}, 2024};

    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto* data = std::get_if<TaxClassificationQuestion>(&run.result().mRequests.at(0).mData);

    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->mClassLabels, (std::vector<std::string>{"FUND", "STOCK"}));
}

TEST(TaxClassifierTest, UnknownInstrumentThatWasOnlyBoughtIsAskedToo) {
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Unknown,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2023-01-10"),
                                         .mSources = {source(2, "synthetic-buy-1")}},
                           .mTradeSide = TradeSide::Buy,
                           .mUnits = kOneUnit,
                           .mCurrency = Currency::EUR,
                           .mInstrumentEvidence = {evidence(2, AssetClass::Unknown, "")}}},
    }};

    const ClassifierRun run{std::move(statement), {}, 2024};

    EXPECT_EQ(run.result().mRequests.size(), 1U);
}

TEST(TaxClassifierTest, UnknownInstrumentWithEventsOnlyAfterTheYearIsNotAsked) {
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Unknown,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-01-10"),
                                         .mSources = {source(2, "synthetic-buy-1")}},
                           .mTradeSide = TradeSide::Buy,
                           .mUnits = kOneUnit,
                           .mCurrency = Currency::EUR,
                           .mInstrumentEvidence = {evidence(2, AssetClass::Unknown, "")}}},
    }};

    const ClassifierRun run{std::move(statement), {}, 2024};

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.result().mDiagnostics.empty());
}

TEST(TaxClassifierTest, CapitalGainsAnswerKeepsTheInstrumentAndClearsTheBlock) {
    ClassifierRun run{unknownShare(""), {}, 2024};

    run.answerFirst(CapitalGainsAnswer{});

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.result().mDiagnostics.empty());
    EXPECT_TRUE(run.result().mExcludedItems.empty());
    EXPECT_TRUE(run.mLast.mLeftOutIsins.empty());
    EXPECT_EQ(run.result().report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);

    ASSERT_EQ(run.result().mDecisions.size(), 1U);
    EXPECT_TRUE(run.result().mDecisions.at(0).mUsed);
    EXPECT_TRUE(std::holds_alternative<CapitalGainsAnswer>(run.result().mDecisions.at(0).mAnswer));
}

TEST(TaxClassifierTest, ExcludeAnswerLeavesTheInstrumentOutAsAUserChoice) {
    ClassifierRun run{unknownShare("MYSTERY"), {}, 2024};

    run.answerFirst(ExcludeFromReportsAnswer{});

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_EQ(run.mLast.mLeftOutIsins, (std::set<Isin>{"XX0000000001"}));

    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);
    EXPECT_EQ(run.result().mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(run.result().mDiagnostics.at(0).mMessageKey,
              MessageKey::TaxCategoryNotReportedUserChoice);

    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);

    const auto& item = run.result().mExcludedItems.at(0);

    EXPECT_EQ(item.mCategory, ExcludedCategory::Unknown);
    EXPECT_EQ(item.mReason, ExcludedReason::UserChoice);
    EXPECT_FALSE(item.isTaxRule());
    EXPECT_EQ(item.mClassLabels, std::vector<std::string>{"MYSTERY"}); // the original label
    EXPECT_EQ(item.mDiagnosticIds, std::vector<std::string>{"diagnostic-0001"});
    EXPECT_EQ(item.mSources.size(), 2U);
}

TEST(TaxClassifierTest, CancelIsABlockingErrorWithoutARequestAndIsNotAnExclusion) {
    ClassifierRun run{unknownShare(""), {}, 2024};

    run.answerFirst(CancelAnswer{});

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.result().mExcludedItems.empty());
    EXPECT_TRUE(run.mLast.mLeftOutIsins.empty());

    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);
    EXPECT_TRUE(run.result().mDiagnostics.at(0).mBlocking);
    EXPECT_EQ(run.result().mDiagnostics.at(0).mMessageKey,
              MessageKey::TaxClassificationUnknownCancelled);
    EXPECT_EQ(run.result().report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
}

TEST(TaxClassifierTest, ASessionRejectsAnAnswerOfAnotherQuestion) {
    ClassifierRun run{unknownShare(""), {}, 2024};

    const auto rejections = run.mSession.answer({DecisionAnswer{
        .mRequestId = run.result().mRequests.at(0).mId,
        .mRunRevision = run.result().mRunRevision,
        .mValue = NoOtherActivityAnswer{},
    }});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::AnswerDoesNotFit);
    EXPECT_EQ(run.result().mRequests.size(), 1U);
}

TEST(TaxClassifierTest, AnAnswerOfAnEarlierRevisionIsStale) {
    ClassifierRun run{unknownShare(""), {}, 2024};

    const auto staleRevision = run.result().mRunRevision;
    const auto requestId = run.result().mRequests.at(0).mId;

    run.answerFirst(CapitalGainsAnswer{});

    const auto rejections = run.mSession.answer({DecisionAnswer{
        .mRequestId = requestId,
        .mRunRevision = staleRevision,
        .mValue = ExcludeFromReportsAnswer{},
    }});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::StaleRevision);
}

TEST(TaxClassifierTest, ParserAndMergerClassErrorsOfAnUnknownInstrumentAreTakenOver) {
    // Rows 2 and 3 built the instrument. The first three diagnostics are about their class; the
    // fourth is the merger's class conflict of the same ISIN; the fifth is another error on
    // row 2; the sixth is a class error on row 9, which no instrument was built from; the
    // seventh is a conflict of another ISIN.
    const auto classError = [](std::size_t aRow, MessageKey aKey) {
        return StatementMergeDiagnostic{SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic = {.mSeverity = DiagnosticSeverity::Error,
                            .mCode = DiagnosticCode::MissingField,
                            .mSourceFile = "history.csv",
                            .mRowIndex = aRow,
                            .mMessageKey = aKey,
                            .mRowKind = ParsedRowKind::Trade},
        }};
    };
    std::vector<StatementMergeDiagnostic> diagnostics{
        classError(2, MessageKey::MissingFieldAssetClass),
        classError(3, MessageKey::UnsupportedAssetClassUnrecognized),
        classError(2, MessageKey::InconsistentValueAssetClass),
        MergeDiagnostic{.mSeverity = DiagnosticSeverity::Error,
                        .mCode = MergeDiagnosticCode::InstrumentAssetClassConflict,
                        .mIsin = "XX0000000001"},
        classError(2, MessageKey::InvalidValueField),
        classError(9, MessageKey::MissingFieldAssetClass),
        MergeDiagnostic{.mSeverity = DiagnosticSeverity::Error,
                        .mCode = MergeDiagnosticCode::InstrumentAssetClassConflict,
                        .mIsin = "XX0000000009"},
    };

    const ClassifierRun run{unknownShare(""), std::move(diagnostics), 2024};

    EXPECT_EQ(run.mLast.mResolvedMergeDiagnostics, (std::vector<std::size_t>{0, 1, 2, 3}));

    // Taken over, the four stay in the result but no longer block; the other three still do.
    const auto blocking = std::count_if(
        run.result().mDiagnostics.begin(),
        run.result().mDiagnostics.end(),
        [](const ApplicationDiagnostic& aDiagnostic) { return aDiagnostic.mBlocking; });

    EXPECT_EQ(blocking, 4); // the other three plus the classification error of the open question
}

TEST(TaxClassifierTest, ClassErrorsOfAKnownClassAreNotTakenOver) {
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2024-01-10"),
                                         .mSources = {source(2, "synthetic-buy-1")}},
                           .mTradeSide = TradeSide::Buy,
                           .mUnits = kOneUnit,
                           .mCurrency = Currency::EUR}},
    }};

    std::vector<StatementMergeDiagnostic> diagnostics{
        StatementMergeDiagnostic{SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic = {.mSeverity = DiagnosticSeverity::Error,
                            .mCode = DiagnosticCode::MissingField,
                            .mSourceFile = "history.csv",
                            .mRowIndex = 2,
                            .mMessageKey = MessageKey::MissingFieldAssetClass,
                            .mRowKind = ParsedRowKind::Trade},
        }},
    };

    const ClassifierRun run{std::move(statement), std::move(diagnostics), 2024};

    EXPECT_TRUE(run.mLast.mResolvedMergeDiagnostics.empty());
}

TEST(TaxClassifierTest, CorporateActionRowsAreSourcesOfTheItemAndOfTheQuestion) {
    MergedStatement statement;

    // The crypto split row (row 3) is in the year, so it is a source of the item; the share has
    // only a split row up to 31 December (row 4) and still gets its question.
    statement.mPresentation.mTradeInstruments = {
        {.mName = "Synthetic Coin",
         .mIsin = "XX0000000001",
         .mAssetClass = AssetClass::Crypto,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2024-01-10"),
                                          .mSources = {source(2, "synthetic-buy-1")}},
                            .mTradeSide = TradeSide::Buy,
                            .mUnits = kOneUnit,
                            .mCurrency = Currency::EUR}},
         .mCorporateActions = {{.mMetadata = {.mTaxDate = day("2024-03-15"),
                                              .mSources = {source(3, "synthetic-split-1")}},
                                .mType = CorporateActionType::UnresolvedSplit,
                                .mUnitsDelta = kOneUnit}}},
        {.mName = "Synthetic Share",
         .mIsin = "XX0000000002",
         .mAssetClass = AssetClass::Unknown,
         .mCorporateActions = {{.mMetadata = {.mTaxDate = day("2023-06-15"),
                                              .mSources = {source(4, "synthetic-split-2")}},
                                .mType = CorporateActionType::UnresolvedSplit,
                                .mUnitsDelta = kOneUnit,
                                .mInstrumentEvidence = {evidence(4, AssetClass::Unknown, "")}}}},
    };

    const ClassifierRun run{std::move(statement), {}, 2024};

    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);
    EXPECT_EQ(run.result().mExcludedItems.at(0).mSources.size(), 2U);
    EXPECT_EQ(run.result().mExcludedItems.at(0).mClassLabels, std::vector<std::string>{});

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mIsin, "XX0000000002");
    EXPECT_EQ(run.result().mRequests.at(0).mTaxDate, day("2023-06-15"));
}

TEST(TaxClassifierTest, BondRowsOutsideTheYearAreNotCountedAndTheEarliestMaturityDateWins) {
    MergedStatement statement;

    // Listed out of date order on purpose: a maturity on 2025-08-21 comes before the one on
    // 2025-08-14. A maturity in 2024 and a purchase on 2026-01-05 are outside the year 2025.
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Bond,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2026-01-05"),
                                         .mSources = {source(2, "synthetic-buy-1")}},
                           .mTradeSide = TradeSide::Buy,
                           .mUnits = 7 * kOneUnit,
                           .mCurrency = Currency::EUR},
                          {.mMetadata = {.mTaxDate = day("2025-02-03"),
                                         .mSources = {source(3, "synthetic-buy-2")}},
                           .mTradeSide = TradeSide::Buy,
                           .mUnits = 5 * kOneUnit,
                           .mCurrency = Currency::EUR}},
    }};
    statement.mPresentation.mBondRedemptions = {
        {.mMetadata = {.mTaxDate = day("2025-08-21"), .mSources = {source(4, "synthetic-4")}},
         .mKind = BondRedemptionKind::Maturity,
         .mName = "Synthetic Bond",
         .mIsin = "XX0000000001",
         .mUnits = 2 * kOneUnit},
        {.mMetadata = {.mTaxDate = day("2025-08-14"), .mSources = {source(5, "synthetic-5")}},
         .mKind = BondRedemptionKind::Maturity,
         .mName = "Synthetic Bond",
         .mIsin = "XX0000000001",
         .mUnits = 3 * kOneUnit},
        {.mMetadata = {.mTaxDate = day("2024-08-14"), .mSources = {source(6, "synthetic-6")}},
         .mKind = BondRedemptionKind::Maturity,
         .mName = "Synthetic Bond",
         .mIsin = "XX0000000001",
         .mUnits = 11 * kOneUnit},
    };

    const ClassifierRun run{std::move(statement), {}, 2025};

    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);

    const auto& item = run.result().mExcludedItems.at(0);

    ASSERT_TRUE(item.mBond.has_value());
    EXPECT_EQ(item.mBond->mBoughtUnits, 5 * kOneUnit);  // not the 7 units of 2026
    EXPECT_EQ(item.mBond->mMaturedUnits, 5 * kOneUnit); // 2 + 3, not the 11 units of 2024
    EXPECT_EQ(item.mBond->mMaturityDate, day("2025-08-14"));
    EXPECT_EQ(item.mSources.size(), 3U); // rows 3, 4 and 5

    // The first date of the events is the warning's date: the purchase on 2025-02-03.
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mTaxDate, day("2025-02-03"));
}

TEST(TaxClassifierTest, CryptoRowsOutsideTheYearGiveNoItem) {
    MergedStatement statement;

    // A purchase in 2026 and a split row in 2023 are outside the year 2025.
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Coin",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Crypto,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2026-01-05"),
                                         .mSources = {source(2, "synthetic-buy-1")}},
                           .mTradeSide = TradeSide::Buy,
                           .mUnits = kOneUnit,
                           .mCurrency = Currency::EUR}},
        .mCorporateActions = {{.mMetadata = {.mTaxDate = day("2023-03-15"),
                                             .mSources = {source(3, "synthetic-split-1")}},
                               .mType = CorporateActionType::UnresolvedSplit,
                               .mUnitsDelta = kOneUnit}},
    }};

    const ClassifierRun run{std::move(statement), {}, 2025};

    EXPECT_EQ(run.mLast.mLeftOutIsins, (std::set<Isin>{"XX0000000001"}));
    EXPECT_TRUE(run.result().mExcludedItems.empty());
    EXPECT_TRUE(run.result().mDiagnostics.empty());
}

} // namespace
