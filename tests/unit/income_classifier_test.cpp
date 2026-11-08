#include "processors/income_classifier.hpp"

#include "processors/interest_payments.hpp"
#include "taxbroker/reference_catalogue.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

// All names, ISINs, dates and amounts in this test suite are synthetic. Money is stored with
// 4 decimals, so 120'000 is EUR 12.00 and 50'000 is EUR 5.00. The synthetic ISINs use the
// prefix XX.

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
        .mName = "Synthetic Payer",
        .mAssetClass = aClass,
        .mAssetClassLabel = aLabel,
    };
}

ReportingYearLedger ledgerOf(int aYear) {
    const auto year = std::to_string(aYear);

    return ReportingYearLedger{.mYear = aYear,
                               .mYearStart = day(year + "-01-01"),
                               .mYearEnd = day(year + "-12-31")};
}

/// Runs the classifier in a session, so a test can answer its questions.
struct IncomeRun {
    DecisionSession mSession;
    MergedStatement mStatement;
    std::vector<StatementMergeDiagnostic> mMergeDiagnostics;
    ReportingYearLedger mLedger;
    IncomeClassificationResult mLast;

    IncomeRun(MergedStatement aStatement,
              std::vector<StatementMergeDiagnostic> aMergeDiagnostics,
              int aYear)
        : mStatement(std::move(aStatement)), mMergeDiagnostics(std::move(aMergeDiagnostics)),
          mLedger(ledgerOf(aYear)) {
        start();
    }

    /// A new run with the current input; the recorded answers stay.
    void start() {
        mSession.start([this](RunDecisions& aDecisions) {
            mLast = IncomeClassifier::classify(mStatement, mLedger, mMergeDiagnostics, aDecisions);

            ApplicationResultInput input{.mReportingYear = mLedger.mYear,
                                         .mMergeDiagnostics = mMergeDiagnostics};
            auto copy = mLast;

            appendIncomeClassification(input, std::move(copy));

            return input;
        });
    }

    [[nodiscard]] const ApplicationResult& result() const {
        return mSession.result();
    }

    /// Answers the open request at aPosition and returns the rejections.
    std::vector<RejectedAnswer> answerAt(std::size_t aPosition, AnswerValue aAnswer) {
        return mSession.answer({DecisionAnswer{
            .mRequestId = result().mRequests.at(aPosition).mId,
            .mRunRevision = result().mRunRevision,
            .mValue = std::move(aAnswer),
        }});
    }
};

ReportStatus statusOf(const ApplicationResult& aResult, ReportType aReport) {
    return aResult.report(aReport).mStatus;
}

constexpr std::string_view kPayersHeader =
    "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
    "source_revision\n";

/// A catalogue with the given payer rows and one broker payer row for Trade Republic.
ReferenceCatalogue catalogueOf(std::string_view aPayerRows) {
    const std::string payers = std::string{kPayersHeader} + std::string{aPayerRows};

    return parseReferenceCatalogue(
        payers,
        "country,statement,source,source_revision\n",
        "broker,code,name,tax_number,address,country,source,source_revision\n"
        "TR,TR-X,Broker T,TAX-T,\"1 Main Street, Berlin\",DE,manual,\n");
}

/// Runs the interest stage on the statement the classifier rewrote.
InterestPaymentsResult interestStageOf(const IncomeRun& aRun,
                                       const ReferenceCatalogue& aCatalogue) {
    const OfficialRateTable rates{};
    DecisionSession session;
    InterestPaymentsResult payments;

    session.start([&](RunDecisions& aDecisions) {
        payments = prepareInterestPayments(*aRun.mLast.mStatement,
                                           aRun.mLedger,
                                           aCatalogue,
                                           rates,
                                           aDecisions);

        return ApplicationResultInput{.mReportingYear = aRun.mLedger.mYear};
    });

    return payments;
}

// --- Questions ---

TEST(IncomeClassifierTest, InterestOfUnknownTypeWithIsinAsksOneQuestionAndBlocksThreeForms) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
            },
    }};

    const IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    EXPECT_EQ(request.mType, RequestType::TaxClassificationRequired);
    EXPECT_EQ(request.mAffectedReports,
              (std::vector<ReportType>{ReportType::Dividends,
                                       ReportType::BankDepositInterest,
                                       ReportType::OtherInterest}));
    EXPECT_EQ(request.mIsin, "XX0000000001");
    EXPECT_EQ(request.mTaxDate, day("2025-03-10"));

    const auto* data = std::get_if<IncomeClassificationQuestion>(&request.mData);

    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->mEventKind, IncomeEventKind::Interest);
    EXPECT_EQ(data->mDate, day("2025-03-10"));
    EXPECT_EQ(data->mName, "Synthetic Payer");
    EXPECT_EQ(data->mIsin, "XX0000000001");
    EXPECT_EQ(data->mClassLabels, std::vector<std::string>{"STOCK"});
    EXPECT_EQ(data->mGrossAmount, 120'000); // EUR 12.00
    EXPECT_EQ(data->mCurrency, Currency::EUR);
    ASSERT_TRUE(data->mTransaction.has_value());
    EXPECT_EQ(data->mTransaction->mTransactionId, "synthetic-int-1");
    EXPECT_TRUE(data->offers(IncomeTreatment::Dividends));
    EXPECT_TRUE(data->offers(IncomeTreatment::BankDepositInterest));
    EXPECT_TRUE(data->offers(IncomeTreatment::BrokerCashInterest));
    EXPECT_TRUE(data->offers(IncomeTreatment::BondCoupon));

    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);

    const auto& diagnostic = run.result().mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_TRUE(diagnostic.mBlocking);
    EXPECT_EQ(diagnostic.mMessageKey,
              MessageKey::TaxClassificationUnknownIncomeConfirmationRequired);
    EXPECT_EQ(diagnostic.mRequestId, request.mId);
    EXPECT_EQ(statusOf(run.result(), ReportType::Dividends), ReportStatus::NeedsInput);
    EXPECT_EQ(statusOf(run.result(), ReportType::BankDepositInterest), ReportStatus::NeedsInput);
    EXPECT_EQ(statusOf(run.result(), ReportType::OtherInterest), ReportStatus::NeedsInput);

    // The open payment is in no form until it is answered.
    ASSERT_TRUE(run.mLast.mStatement.has_value());
    EXPECT_TRUE(
        run.mLast.mStatement->mPresentation.mInterestInstruments.at(0).mTransactions.empty());
}

TEST(IncomeClassifierTest, WithoutAnIsinNoDividendAndNoCouponIsOfferedAndDohDivIsNotBlocked) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Fund, "FUND")}},
            },
    }};

    const IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    EXPECT_EQ(
        request.mAffectedReports,
        (std::vector<ReportType>{ReportType::BankDepositInterest, ReportType::OtherInterest}));
    EXPECT_FALSE(request.mIsin.has_value());

    const auto* data = std::get_if<IncomeClassificationQuestion>(&request.mData);

    ASSERT_NE(data, nullptr);
    EXPECT_FALSE(data->offers(IncomeTreatment::Dividends));
    EXPECT_TRUE(data->offers(IncomeTreatment::BankDepositInterest));
    EXPECT_TRUE(data->offers(IncomeTreatment::BrokerCashInterest));
    EXPECT_FALSE(data->offers(IncomeTreatment::BondCoupon));
    // No dividend exists and none can be this payment, so Doh-Div has nothing to report.
    EXPECT_EQ(statusOf(run.result(), ReportType::Dividends), ReportStatus::NoData);
    EXPECT_EQ(statusOf(run.result(), ReportType::BankDepositInterest), ReportStatus::NeedsInput);
    EXPECT_EQ(statusOf(run.result(), ReportType::OtherInterest), ReportStatus::NeedsInput);
}

TEST(IncomeClassifierTest, ASessionRefusesATreatmentTheQuestionDoesNotOffer) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Fund, "FUND")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    const auto dividend = run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::Dividends});
    const auto coupon = run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::BondCoupon});
    const auto capital = run.answerAt(0, CapitalGainsAnswer{});

    EXPECT_EQ(dividend.size(), 1U); // a dividend row needs an ISIN
    EXPECT_EQ(coupon.size(), 1U);   // a coupon needs the ISIN of the issuer
    EXPECT_EQ(capital.size(), 1U);  // income is never capital gains
    EXPECT_EQ(run.result().mRequests.size(), 1U);
}

TEST(IncomeClassifierTest, APaymentOutsideTheReportingYearIsNotAsked) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-12-31"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
            },
    }};

    const IncomeRun run{std::move(statement), {}, 2025};

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.result().mDiagnostics.empty());
    EXPECT_FALSE(run.mLast.mStatement.has_value());
}

TEST(IncomeClassifierTest, PaymentsBeforeAndAfterTheReportingYearAreNotAsked) {
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-12-31"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Unknown, "")}},
                {.mMetadata = {.mTaxDate = day("2026-01-01"),
                               .mSources = {source(3, "synthetic-div-2")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(3, AssetClass::Unknown, "")}},
            },
    }};
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2026-01-01"),
                               .mSources = {source(4, "synthetic-int-1")}},
                 .mGrossAmount = 30'000, // EUR 3.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(4, AssetClass::Stock, "STOCK")}},
            },
    }};

    const IncomeRun run{std::move(statement), {}, 2025};

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.result().mDiagnostics.empty());
    EXPECT_FALSE(run.mLast.mStatement.has_value());
}

TEST(IncomeClassifierTest, ADividendOfAShareAsksNothing) {
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
                {.mMetadata = {.mTaxDate = day("2025-06-10"),
                               .mSources = {source(3, "synthetic-div-2")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(3, AssetClass::Fund, "FUND")}},
            },
    }};

    const IncomeRun run{std::move(statement), {}, 2025};

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.result().mDiagnostics.empty());
    EXPECT_FALSE(run.mLast.mStatement.has_value());
}

TEST(IncomeClassifierTest, ADividendWithABlankClassAsksAndKeepsTheBlankLabel) {
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Unknown, "")}},
            },
    }};

    const IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto* data =
        std::get_if<IncomeClassificationQuestion>(&run.result().mRequests.at(0).mData);

    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->mEventKind, IncomeEventKind::Dividend);
    EXPECT_EQ(data->mClassLabels, std::vector<std::string>{""});
    EXPECT_EQ(statusOf(run.result(), ReportType::Dividends), ReportStatus::NeedsInput);
}

TEST(IncomeClassifierTest, ADividendOfABondAsksBecauseABondPaysCoupons) {
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Bond, "BOND")}},
            },
    }};

    const IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto* data =
        std::get_if<IncomeClassificationQuestion>(&run.result().mRequests.at(0).mData);

    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->mClassLabels, std::vector<std::string>{"BOND"});
}

TEST(IncomeClassifierTest, ConflictingClassesOfOneInstrumentAskForEveryDividend) {
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
                {.mMetadata = {.mTaxDate = day("2025-06-10"),
                               .mSources = {source(3, "synthetic-div-2")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(3, AssetClass::Unknown, "")}},
            },
    }};

    const IncomeRun run{std::move(statement), {}, 2025};

    // Both dividends are asked, also the one whose own row says STOCK.
    ASSERT_EQ(run.result().mRequests.size(), 2U);
    EXPECT_EQ(run.result().mRequests.at(0).mTaxDate, day("2025-03-10"));
    EXPECT_EQ(run.result().mRequests.at(1).mTaxDate, day("2025-06-10"));
}

// --- Answers ---

TEST(IncomeClassifierTest, AnswerBrokerCashMovesThePaymentToDohObrAndKeepsTheBlankTax) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
                {.mMetadata = {.mTaxDate = day("2025-04-10"),
                               .mSources = {source(3, "synthetic-int-2")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(3, AssetClass::Stock, "STOCK")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_EQ(run.result().mRequests.size(), 2U);
    ASSERT_TRUE(
        run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::BrokerCashInterest}).empty());
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_TRUE(
        run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::BrokerCashInterest}).empty());

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.result().mDiagnostics.empty());

    // Both payments left the unknown instrument and joined one broker-cash instrument.
    const auto& instruments = run.mLast.mStatement->mPresentation.mInterestInstruments;

    ASSERT_EQ(instruments.size(), 2U);
    EXPECT_TRUE(instruments.at(0).mTransactions.empty());
    EXPECT_EQ(instruments.at(1).mInterestType, InterestType::BrokerInterest);
    EXPECT_FALSE(instruments.at(1).mIsin.has_value());

    ASSERT_EQ(instruments.at(1).mTransactions.size(), 2U);

    const auto& blank = instruments.at(1).mTransactions.at(0);
    const auto& zero = instruments.at(1).mTransactions.at(1);

    EXPECT_EQ(blank.mGrossAmount, 120'000);   // EUR 12.00
    EXPECT_FALSE(blank.mTaxPaid.has_value()); // a blank tax stays blank
    EXPECT_EQ(blank.mCashHolder, CashHolder::Broker);
    EXPECT_EQ(zero.mGrossAmount, 120'000); // EUR 12.00
    EXPECT_EQ(zero.mTaxPaid, 0);           // an explicit zero stays an explicit zero
    EXPECT_EQ(zero.mCashHolder, CashHolder::Broker);
}

TEST(IncomeClassifierTest, TheRewrittenStatementFeedsTheInterestStageAfterABrokerCashAnswer) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(
        run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::BrokerCashInterest}).empty());

    const auto catalogue = parseReferenceCatalogue(
        "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
        "source_revision\n",
        "country,statement,source,source_revision\n",
        "broker,code,name,tax_number,address,country,source,source_revision\n");
    const OfficialRateTable rates{};
    DecisionSession session;
    InterestPaymentsResult payments;

    session.start([&](RunDecisions& aDecisions) {
        payments = prepareInterestPayments(*run.mLast.mStatement,
                                           run.mLedger,
                                           catalogue,
                                           rates,
                                           aDecisions);

        return ApplicationResultInput{.mReportingYear = 2025};
    });

    // One Doh-Obr row of broker cash interest, EUR 12.00 gross and no foreign tax.
    ASSERT_EQ(payments.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(payments.mOtherReport.mRows.at(0).mGrossInterest, 120'000);
    EXPECT_TRUE(
        std::holds_alternative<BrokerCashInterest>(payments.mOtherReport.mRows.at(0).mKind));
    EXPECT_EQ(payments.mOtherReport.mRows.at(0).mDate, day("2025-03-10"));
}

TEST(IncomeClassifierTest, AnswerDividendsMovesAnInterestPaymentToTheDividendInstrumentOfItsIsin) {
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-10"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 30'000, // EUR 3.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
                {.mMetadata = {.mTaxDate = day("2025-05-10"),
                               .mSources = {source(4, "synthetic-div-3")}},
                 .mGrossAmount = 40'000, // EUR 4.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(4, AssetClass::Stock, "STOCK")}},
            },
    }};
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(3, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(3, AssetClass::Stock, "STOCK")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::Dividends}).empty());

    const auto& presentation = run.mLast.mStatement->mPresentation;

    EXPECT_TRUE(presentation.mInterestInstruments.at(0).mTransactions.empty());
    ASSERT_EQ(presentation.mDividendInstruments.size(), 1U);

    // The moved payment sits between the two dividends, by its date.
    const auto& dividends = presentation.mDividendInstruments.at(0).mTransactions;

    ASSERT_EQ(dividends.size(), 3U);
    EXPECT_EQ(dividends.at(0).mGrossAmount, 30'000);  // EUR 3.00, 2025-01-10
    EXPECT_EQ(dividends.at(1).mGrossAmount, 120'000); // EUR 12.00, 2025-03-10
    EXPECT_EQ(dividends.at(1).mTaxPaid, 0);
    EXPECT_EQ(dividends.at(2).mGrossAmount, 40'000); // EUR 4.00, 2025-05-10

    // The chronological order was built again from the new positions.
    const auto& order = run.mLast.mStatement->mChronologicalOrder;

    ASSERT_EQ(order.size(), 3U);
    EXPECT_EQ(order.at(1).mKind, StatementEventKind::Dividend);
    EXPECT_EQ(order.at(1).mEventIndex, 1U);
}

TEST(IncomeClassifierTest, AnswerDepositForADividendOfUnknownClassMakesItBankDepositInterest) {
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Unknown, "")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(
        run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::BankDepositInterest}).empty());

    const auto& presentation = run.mLast.mStatement->mPresentation;

    EXPECT_TRUE(presentation.mDividendInstruments.at(0).mTransactions.empty());
    ASSERT_EQ(presentation.mInterestInstruments.size(), 1U);
    EXPECT_EQ(presentation.mInterestInstruments.at(0).mInterestType, InterestType::BrokerInterest);
    ASSERT_EQ(presentation.mInterestInstruments.at(0).mTransactions.size(), 1U);
    EXPECT_EQ(presentation.mInterestInstruments.at(0).mTransactions.at(0).mCashHolder,
              CashHolder::Bank);
    EXPECT_EQ(presentation.mInterestInstruments.at(0).mTransactions.at(0).mGrossAmount, 120'000);
}

TEST(IncomeClassifierTest, AnswerDividendsForADividendOfUnknownClassKeepsItWhereItIs) {
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Unknown, "")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::Dividends}).empty());

    const auto& presentation = run.mLast.mStatement->mPresentation;

    ASSERT_EQ(presentation.mDividendInstruments.size(), 1U);
    ASSERT_EQ(presentation.mDividendInstruments.at(0).mTransactions.size(), 1U);
    EXPECT_TRUE(presentation.mInterestInstruments.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.result().mDiagnostics.empty());
}

TEST(IncomeClassifierTest, AnswerBondCouponMovesThePaymentToTheBondInstrumentOfItsIsin) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {
        {
            .mName = "Synthetic Bond",
            .mIsin = "XX0000000010",
            .mInterestType = InterestType::BondInterest,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2025-01-10"),
                                   .mSources = {source(2, "synthetic-int-1")}},
                     .mGrossAmount = 30'000, // EUR 3.00
                     .mTaxPaid = std::nullopt,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR,
                     .mInstrumentEvidence = {evidence(2, AssetClass::Bond, "BOND")}},
                },
        },
        {
            .mName = "Synthetic Bond",
            .mIsin = "XX0000000010",
            .mInterestType = InterestType::UnknownInterest,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2025-07-10"),
                                   .mSources = {source(3, "synthetic-int-2")}},
                     .mGrossAmount = 30'000, // EUR 3.00
                     .mTaxPaid = std::nullopt,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR,
                     .mInstrumentEvidence = {evidence(3, AssetClass::Fund, "FUND")}},
                },
        },
    };

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::BondCoupon}).empty());

    const auto& instruments = run.mLast.mStatement->mPresentation.mInterestInstruments;

    // No third instrument: the coupon joins the bond instrument that is already there.
    ASSERT_EQ(instruments.size(), 2U);
    ASSERT_EQ(instruments.at(0).mTransactions.size(), 2U);
    EXPECT_EQ(instruments.at(0).mTransactions.at(1).mMetadata.mTaxDate, day("2025-07-10"));
    EXPECT_FALSE(instruments.at(0).mTransactions.at(1).mCashHolder.has_value());
    EXPECT_TRUE(instruments.at(1).mTransactions.empty());
}

TEST(IncomeClassifierTest, AnswerDividendsCreatesTheDividendInstrumentWhenThereIsNone) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::Dividends}).empty());

    const auto& dividends = run.mLast.mStatement->mPresentation.mDividendInstruments;

    ASSERT_EQ(dividends.size(), 1U);
    EXPECT_EQ(dividends.at(0).mName, "Synthetic Payer");
    EXPECT_EQ(dividends.at(0).mIsin, "XX0000000001");
    ASSERT_EQ(dividends.at(0).mTransactions.size(), 1U);
    EXPECT_EQ(dividends.at(0).mTransactions.at(0).mGrossAmount, 120'000); // EUR 12.00
}

TEST(IncomeClassifierTest, AnswerBondCouponCreatesTheBondInstrumentWhenThereIsNone) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-07-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 30'000, // EUR 3.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Fund, "FUND")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::BondCoupon}).empty());

    const auto& instruments = run.mLast.mStatement->mPresentation.mInterestInstruments;

    ASSERT_EQ(instruments.size(), 2U);
    EXPECT_TRUE(instruments.at(0).mTransactions.empty());
    EXPECT_EQ(instruments.at(1).mInterestType, InterestType::BondInterest);
    EXPECT_EQ(instruments.at(1).mIsin, "XX0000000010");
    ASSERT_EQ(instruments.at(1).mTransactions.size(), 1U);
    EXPECT_FALSE(instruments.at(1).mTransactions.at(0).mCashHolder.has_value());
    EXPECT_EQ(instruments.at(1).mTransactions.at(0).mTaxPaid, 0); // an explicit zero stays
}

TEST(IncomeClassifierTest, ExcludingListsTheUserChoiceAndRemovesThePaymentFromEveryForm) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(run.answerAt(0, ExcludeFromReportsAnswer{}).empty());

    EXPECT_TRUE(run.result().mRequests.empty());

    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);
    EXPECT_EQ(run.result().mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_FALSE(run.result().mDiagnostics.at(0).mBlocking);
    EXPECT_EQ(run.result().mDiagnostics.at(0).mMessageKey,
              MessageKey::TaxCategoryNotReportedUserChoiceIncome);

    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);

    const auto& item = run.result().mExcludedItems.at(0);

    EXPECT_EQ(item.mIsin, "XX0000000001");
    EXPECT_EQ(item.mName, "Synthetic Payer");
    EXPECT_EQ(item.mCategory, ExcludedCategory::Unknown);
    EXPECT_EQ(item.mReason, ExcludedReason::UserChoice);
    EXPECT_EQ(item.mClassLabels, std::vector<std::string>{"STOCK"});
    ASSERT_EQ(item.mDiagnosticIds.size(), 1U);
    EXPECT_EQ(item.mDiagnosticIds.at(0), run.result().mDiagnostics.at(0).mId);
    EXPECT_TRUE(
        run.mLast.mStatement->mPresentation.mInterestInstruments.at(0).mTransactions.empty());
}

TEST(IncomeClassifierTest, CancelBlocksTheFormsAndIsNotAnExclusion) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(run.answerAt(0, CancelAnswer{}).empty());

    // The question is closed and no longer open; its error has no request, so the forms fail.
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.result().mExcludedItems.empty());

    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);
    EXPECT_EQ(run.result().mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_TRUE(run.result().mDiagnostics.at(0).mBlocking);
    EXPECT_EQ(run.result().mDiagnostics.at(0).mMessageKey,
              MessageKey::TaxClassificationUnknownIncomeCancelled);
    EXPECT_EQ(statusOf(run.result(), ReportType::Dividends), ReportStatus::Failed);
    EXPECT_EQ(statusOf(run.result(), ReportType::BankDepositInterest), ReportStatus::Failed);
    EXPECT_EQ(statusOf(run.result(), ReportType::OtherInterest), ReportStatus::Failed);
}

// --- Several payments ---

TEST(IncomeClassifierTest, PaymentsOfOneInstrumentCanGetDifferentTreatments) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
                {.mMetadata = {.mTaxDate = day("2025-06-10"),
                               .mSources = {source(3, "synthetic-int-2")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(3, AssetClass::Stock, "STOCK")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    // Two payments are two questions, in date order.
    ASSERT_EQ(run.result().mRequests.size(), 2U);

    ASSERT_TRUE(run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::Dividends}).empty());
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_TRUE(run.answerAt(0, ExcludeFromReportsAnswer{}).empty());

    const auto& presentation = run.mLast.mStatement->mPresentation;

    ASSERT_EQ(presentation.mDividendInstruments.size(), 1U);
    ASSERT_EQ(presentation.mDividendInstruments.at(0).mTransactions.size(), 1U);
    EXPECT_EQ(presentation.mDividendInstruments.at(0).mTransactions.at(0).mGrossAmount,
              120'000);                                // EUR 12.00 is a dividend
    ASSERT_EQ(run.result().mExcludedItems.size(), 1U); // EUR 5.00 is left out
    EXPECT_EQ(run.result().mExcludedItems.at(0).mSources.size(), 1U);
}

TEST(IncomeClassifierTest, TwoPaymentsWithTheSameFactsAreOneQuestionWithTwoLinkedErrors) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"), .mSources = {source(2, "")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
                {.mMetadata = {.mTaxDate = day("2025-03-10"), .mSources = {source(3, "")}},
                 .mGrossAmount = 120'000, // EUR 12.00, the same day and amount
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(3, AssetClass::Stock, "STOCK")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mSources.size(), 2U);
    EXPECT_EQ(run.result().mDiagnostics.size(), 2U);

    ASSERT_TRUE(
        run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::BrokerCashInterest}).empty());

    // One answer moves both payments.
    EXPECT_EQ(run.mLast.mStatement->mPresentation.mInterestInstruments.at(1).mTransactions.size(),
              2U);
}

TEST(IncomeClassifierTest, ADifferentAmountIsANewQuestionAndTheOldAnswerDoesNotApply) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(
        run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::BrokerCashInterest}).empty());
    ASSERT_TRUE(run.result().mRequests.empty());

    // The file now says EUR 12.50 for the same payment: the recorded answer is for another fact.
    run.mStatement.mPresentation.mInterestInstruments.at(0).mTransactions.at(0).mGrossAmount =
        125'000; // EUR 12.50
    run.start();

    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto* data =
        std::get_if<IncomeClassificationQuestion>(&run.result().mRequests.at(0).mData);

    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->mGrossAmount, 125'000);
    EXPECT_EQ(statusOf(run.result(), ReportType::OtherInterest), ReportStatus::NeedsInput);
}

// --- The parse error ---

TEST(IncomeClassifierTest, TheParseErrorOfAnUnknownInterestRowIsTakenOver) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
            },
    }};

    // Position 0: the interest-type error of row 2 (the payment above). Position 1: the same
    // error of row 9, which no payment came from. Position 2: another error of row 2.
    std::vector<StatementMergeDiagnostic> mergeDiagnostics{
        SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic = {.mSeverity = DiagnosticSeverity::Error,
                            .mCode = DiagnosticCode::UnsupportedAssetClass,
                            .mRowIndex = 2,
                            .mMessageKey = MessageKey::UnsupportedAssetClassInterestMapping}},
        SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic = {.mSeverity = DiagnosticSeverity::Error,
                            .mCode = DiagnosticCode::UnsupportedAssetClass,
                            .mRowIndex = 9,
                            .mMessageKey = MessageKey::UnsupportedAssetClassInterestMapping}},
        SourcedParseDiagnostic{.mSourceIndex = 0,
                               .mBroker = Broker::TradeRepublic,
                               .mDiagnostic = {.mSeverity = DiagnosticSeverity::Error,
                                               .mCode = DiagnosticCode::MissingField,
                                               .mRowIndex = 2,
                                               .mMessageKey = MessageKey::MissingFieldName}},
    };

    const IncomeRun run{std::move(statement), std::move(mergeDiagnostics), 2025};

    EXPECT_EQ(run.mLast.mResolvedMergeDiagnostics, std::vector<std::size_t>{0});
}

TEST(IncomeClassifierTest, AnswerDepositCountsForTheAllowanceAndTheBrokerIsThePayer) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {
        {
            .mName = "Synthetic Broker Cash",
            .mIsin = std::nullopt,
            .mInterestType = InterestType::BrokerInterest,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2025-04-10"),
                                   .mSources = {source(3, "synthetic-int-2")}},
                     .mGrossAmount = 7'000'000, // EUR 700.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR,
                     .mCashHolder = CashHolder::Bank},
                },
        },
        {
            .mName = "Synthetic Payer",
            .mIsin = "XX0000000001",
            .mInterestType = InterestType::UnknownInterest,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2025-03-10"),
                                   .mSources = {source(2, "synthetic-int-1")}},
                     .mGrossAmount = 4'000'000, // EUR 400.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR,
                     .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
                },
        },
    };

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(
        run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::BankDepositInterest}).empty());

    const auto catalogue = catalogueOf("");
    const auto payments = interestStageOf(run, catalogue);

    // EUR 400.00 + EUR 700.00 = EUR 1,100.00 is above the EUR 1,000.00 allowance, so the form
    // is made without a question.
    EXPECT_EQ(payments.mQualifyingTotal, std::optional<Money>{11'000'000});
    EXPECT_TRUE(payments.mRequests.empty());
    ASSERT_EQ(payments.mDepositReport.mRows.size(), 1U);

    // The payer is the broker that holds the cash, not the instrument named in the row.
    const auto& row = payments.mDepositReport.mRows.at(0);

    EXPECT_EQ(row.mInterest, 11'000'000);
    EXPECT_EQ(row.mPayer.mName, std::optional<std::string>{"Broker T"});
}

TEST(IncomeClassifierTest, AnswerBondCouponOfASlovenianIssuerIsAnExcludedItem) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Issuer SI",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Bond, "BOND")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(run.answerAt(0, IncomeTreatmentAnswer{IncomeTreatment::BondCoupon}).empty());

    const auto catalogue =
        catalogueOf("XX0000000010,Issuer SI,TAX-S,1 Main Street,SI,,,issuer,manual,\n");
    const auto payments = interestStageOf(run, catalogue);

    // A Slovenian issuer deducts the tax itself: no Doh-Obr row, one excluded item.
    EXPECT_TRUE(payments.mOtherReport.mRows.empty());
    ASSERT_EQ(payments.mExcludedItems.size(), 1U);

    const auto& item = payments.mExcludedItems.at(0).mItem;

    EXPECT_EQ(item.mIsin, "XX0000000010");
    EXPECT_EQ(item.mCategory, ExcludedCategory::Interest);
    EXPECT_EQ(item.mReason, ExcludedReason::SlovenianTaxSettled);
}

TEST(IncomeClassifierTest, TheParseErrorOfAnUnknownInterestRowOutsideTheYearIsTakenOverToo) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-12-31"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
            },
    }};
    std::vector<StatementMergeDiagnostic> mergeDiagnostics{SourcedParseDiagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic = {.mSeverity = DiagnosticSeverity::Error,
                        .mCode = DiagnosticCode::UnsupportedAssetClass,
                        .mRowIndex = 2,
                        .mMessageKey = MessageKey::UnsupportedAssetClassInterestMapping}}};

    const IncomeRun run{std::move(statement), std::move(mergeDiagnostics), 2025};

    // The payment belongs to 2024, so it is in no form of 2025: no question, no blocking error.
    EXPECT_EQ(run.mLast.mResolvedMergeDiagnostics, std::vector<std::size_t>{0});
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_FALSE(run.mLast.mStatement.has_value());
}

TEST(IncomeClassifierTest, ExcludingASharePaymentWithoutAnIsinListsItAndNotesTheMissingIsin) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Stock, "STOCK")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(run.answerAt(0, ExcludeFromReportsAnswer{}).empty());

    // A share should have an ISIN, so the warning says it is missing.
    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);
    EXPECT_EQ(run.result().mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(run.result().mDiagnostics.at(0).mMessageKey,
              MessageKey::TaxCategoryNotReportedUserChoiceIncomeMissingIsin);
    EXPECT_EQ(
        run.result().mDiagnostics.at(0).mAffectedReports,
        (std::vector<ReportType>{ReportType::BankDepositInterest, ReportType::OtherInterest}));

    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);

    const auto& item = run.result().mExcludedItems.at(0);

    EXPECT_EQ(item.mIsin, ""); // Written without the isin key.
    EXPECT_EQ(item.mName, "Synthetic Payer");
    EXPECT_EQ(item.mCategory, ExcludedCategory::Unknown);
    EXPECT_EQ(item.mReason, ExcludedReason::UserChoice);
    EXPECT_EQ(item.mClassLabels, std::vector<std::string>{"STOCK"});
}

TEST(IncomeClassifierTest, ExcludingAPaymentOfUnknownClassWithoutAnIsinNotesNothingAboutTheIsin) {
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-int-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Unknown, "MYSTERY")}},
            },
    }};

    IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_TRUE(run.answerAt(0, ExcludeFromReportsAnswer{}).empty());

    // The class is not a share, fund or bond, so no ISIN is expected.
    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);
    EXPECT_EQ(run.result().mDiagnostics.at(0).mMessageKey,
              MessageKey::TaxCategoryNotReportedUserChoiceIncome);
    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);
    EXPECT_EQ(run.result().mExcludedItems.at(0).mIsin, "");
}

TEST(IncomeClassifierTest, ADividendWithABlankIsinOffersNoDividendAndNoCoupon) {
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer",
        .mIsin = "",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-10"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 120'000, // EUR 12.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mInstrumentEvidence = {evidence(2, AssetClass::Unknown, "")}},
            },
    }};

    const IncomeRun run{std::move(statement), {}, 2025};

    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    EXPECT_FALSE(request.mIsin.has_value());
    EXPECT_EQ(
        request.mAffectedReports,
        (std::vector<ReportType>{ReportType::BankDepositInterest, ReportType::OtherInterest}));

    const auto* data = std::get_if<IncomeClassificationQuestion>(&request.mData);

    ASSERT_NE(data, nullptr);
    EXPECT_FALSE(data->mIsin.has_value());
    EXPECT_FALSE(data->offers(IncomeTreatment::Dividends));
    EXPECT_FALSE(data->offers(IncomeTreatment::BondCoupon));
}

} // namespace
