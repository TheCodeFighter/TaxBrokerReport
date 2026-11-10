#include "processors/failure_resolution_stage.hpp"
#include "processors/interest_payments.hpp"

#include "taxbroker/currency.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

// All payers, ISINs, dates, amounts and rates in this test suite are synthetic. The synthetic
// ISINs use the prefix XX and a valid check digit. Money has 4 decimals, so 50'000 is EUR 5.00;
// units have 8 decimals.

constexpr Units kOneUnit = 100'000'000;

constexpr std::string_view kPayersHeader =
    "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
    "source_revision\n";
constexpr std::string_view kReliefHeader = "country,statement,source,source_revision\n";
constexpr std::string_view kBrokerHeader =
    "broker,code,name,tax_number,address,country,source,source_revision\n";

/// Both issuers are French, so no payer is Slovenian.
constexpr std::string_view kFrenchIssuers =
    "XX0000000010,Issuer A,TAX-A,1 Main Street,FR,,,issuer,manual,\n"
    "XX0000000028,Issuer B,TAX-B,2 Main Street,FR,,,issuer,manual,\n";

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

/// A catalogue with the given issuer rows and one broker payer for Trade Republic.
ReferenceCatalogue catalogueOf(std::string_view aPayerRows) {
    const std::string payers = std::string{kPayersHeader} + std::string{aPayerRows};
    const std::string relief = std::string{kReliefHeader} + "BE,Treaty text,manual,\n";
    const std::string broker = std::string{kBrokerHeader} +
                               "TR,TR-X,Broker T,TAX-T,\"1 Main Street, Berlin\",DE,manual,\n";

    return parseReferenceCatalogue(payers, relief, broker);
}

SourceReference sourceOf(Broker aBroker, std::size_t aRow, std::string aTransactionId) {
    return SourceReference{
        .mBroker = aBroker,
        .mFilename = SourceFilename::fromPath("history.csv"),
        .mSourceRow = aRow,
        .mTransactionId = std::move(aTransactionId),
    };
}

/// A catalogue with the given issuer rows and one broker payer for each of two brokers.
ReferenceCatalogue catalogueOfTwoBrokers(std::string_view aPayerRows) {
    const std::string payers = std::string{kPayersHeader} + std::string{aPayerRows};
    const std::string relief = std::string{kReliefHeader} + "BE,Treaty text,manual,\n";
    const std::string broker = std::string{kBrokerHeader} +
                               "TR,TR-X,Broker T,TAX-T,\"1 Main Street, Berlin\",DE,manual,\n"
                               "IBKR,IB-X,Broker I,TAX-I,\"2 Main Street, Dublin\",IE,manual,\n";

    return parseReferenceCatalogue(payers, relief, broker);
}

/// Marks the rate files of 2025 as loaded, from 2 January to 31 December, without any rate.
void addFile2025(OfficialRateTable& aTable) {
    aTable.mFiles.push_back(OfficialRateFile{.mYear = 2025,
                                             .mFirstDate = day("2025-01-02"),
                                             .mLastDate = day("2025-12-31")});
}

/// Adds one rate; aValue is foreign units per 1 EUR at 8 decimals.
void addRate(OfficialRateTable& aTable,
             std::string_view aCode,
             std::string_view aDate,
             ExchangeRate aValue,
             std::size_t aRow) {
    aTable.mRates[{std::string{aCode}, day(aDate)}] =
        OfficialRate{.mCurrencyCode = std::string{aCode},
                     .mObservationDate = day(aDate),
                     .mValue = aValue,
                     .mSourceFile = SourceFilename::fromPath("exr_2025.csv"),
                     .mSourceRow = aRow,
                     .mSource = OfficialRateSource::EcbReferenceRateFile,
                     .mOrigin = OfficialRateOrigin::Supplied};
}

/// Puts all interest payments in tax-date order. Equal dates keep the order listed in the test.
void orderChronologically(MergedStatement& aStatement) {
    const auto& instruments = aStatement.mPresentation.mInterestInstruments;
    auto& order = aStatement.mChronologicalOrder;

    for (std::size_t instrument = 0; instrument < instruments.size(); ++instrument)
    {
        for (std::size_t payment = 0; payment < instruments[instrument].mTransactions.size();
             ++payment)
        {
            order.push_back({StatementEventKind::Interest, instrument, payment});
        }
    }

    const auto taxDate = [&instruments](const StatementEventReference& aReference) {
        return instruments.at(aReference.mInstrumentIndex.value())
            .mTransactions.at(aReference.mEventIndex)
            .mMetadata.mTaxDate;
    };

    std::stable_sort(order.begin(), order.end(), [&taxDate](const auto& aLeft, const auto& aRight) {
        return taxDate(aLeft) < taxDate(aRight);
    });
}

ReportingYearLedger ledger2025() {
    return ReportingYearLedger{.mYear = 2025,
                               .mYearStart = day("2025-01-01"),
                               .mYearEnd = day("2025-12-31")};
}

/// Runs the interest stage and the Doh-Obr failure stage in a session, as the report command does
/// once it makes Doh-Obr: the rows of the excluded ISINs are removed after the answer. A form with
/// rows gets a placeholder XML, so a status of `generated` shows that nothing blocks it.
struct ObrRun {
    DecisionSession mSession;
    InterestPaymentsResult mLast;
    std::vector<Isin> mExcluded;
    std::vector<Broker> mExcludedBrokers;

    ObrRun(const MergedStatement& aStatement,
           const ReferenceCatalogue& aCatalogue,
           const OfficialRateTable& aRates) {
        start(aStatement, aCatalogue, aRates);
    }

    /// A new run with new input; the recorded answers stay.
    void start(const MergedStatement& aStatement,
               const ReferenceCatalogue& aCatalogue,
               const OfficialRateTable& aRates) {
        mSession.start([this, &aStatement, &aCatalogue, &aRates](RunDecisions& aDecisions) {
            mLast =
                prepareInterestPayments(aStatement, ledger2025(), aCatalogue, aRates, aDecisions);

            ApplicationResultInput input{.mReportingYear = 2025};
            auto copy = mLast;

            appendInterestPayments(input, std::move(copy));
            const auto units =
                FailureResolutionStage::resolveUnits(input, aDecisions, kOtherInterestFailure);

            mExcluded = units.mIsins;
            mExcludedBrokers = units.mBrokers;
            excludeCoupons(mLast.mOtherReport, mExcluded);
            excludeBrokerCashInterest(mLast.mOtherReport, mExcludedBrokers);

            if (!mLast.mOtherReport.mRows.empty())
            {
                input.mReports[static_cast<std::size_t>(ReportType::OtherInterest)].mXml =
                    "<Envelope/>";
            }

            return input;
        });
    }

    [[nodiscard]] const ApplicationResult& result() const {
        return mSession.result();
    }

    [[nodiscard]] ReportStatus obrStatus() const {
        return result().report(ReportType::OtherInterest).mStatus;
    }

    /// Answers the first open request and returns the rejections.
    std::vector<RejectedAnswer> answerFirst(AnswerValue aAnswer) {
        return mSession.answer({DecisionAnswer{
            .mRequestId = result().mRequests.at(0).mId,
            .mRunRevision = result().mRunRevision,
            .mValue = std::move(aAnswer),
        }});
    }
};

const AnswerValue kExclude = FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments};
const AnswerValue kFailReport = FailureResolutionAnswer{FailureResolution::FailReport};

// --- A coupon error of one ISIN ---

TEST(OtherInterestFailureTest, ACouponWithoutARateIsAnErrorOfItsIsinAndAsksTheFailureQuestion) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates); // loaded, but without any USD rate
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 100'000, // USD 10.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
        {.mName = "Synthetic Bond B",
         .mIsin = "XX0000000028",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                                          .mSources = {source(3, "synthetic-int-2")}},
                            .mGrossAmount = 50'000, // EUR 5.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::EUR,
                            .mTaxCurrency = Currency::EUR}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    // One error, made for the coupon of Bond A: its ISIN, Doh-Obr only.
    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);

    const auto& error = run.mLast.mDiagnostics.at(0);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(error.mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_EQ(error.mContext.mAffectedReports, std::vector<ReportType>{ReportType::OtherInterest});
    EXPECT_EQ(error.mContext.mSources.size(), 1U);

    // The question lists Bond A with the reason of its error; Doh-Obr waits for the answer.
    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    EXPECT_EQ(request.mType, RequestType::OtherInterestFailureResolutionRequired);
    ASSERT_EQ(request.mInvestments.size(), 1U);
    EXPECT_EQ(request.mInvestments.at(0).mIsin, "XX0000000010");
    EXPECT_EQ(request.mInvestments.at(0).mName, "Synthetic Bond A");
    EXPECT_EQ(
        request.mInvestments.at(0).mReasonCodes,
        std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::MissingExchangeRate});
    EXPECT_EQ(run.obrStatus(), ReportStatus::NeedsInput);

    // Bond B has its row: EUR 5.00.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 50'000);
}

TEST(OtherInterestFailureTest, ExcludingTheIsinKeepsTheOtherCouponAndTheCashInterest) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 100'000, // USD 10.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
        {.mName = "Synthetic Bond B",
         .mIsin = "XX0000000028",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                                          .mSources = {source(3, "synthetic-int-2")}},
                            .mGrossAmount = 50'000, // EUR 5.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::EUR,
                            .mTaxCurrency = Currency::EUR}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-16"),
                                          .mSources = {source(4, "synthetic-int-3")}},
                            .mGrossAmount = 20'000, // EUR 2.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::EUR,
                            .mTaxCurrency = Currency::EUR,
                            .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(kExclude);

    EXPECT_TRUE(rejections.empty());
    EXPECT_EQ(run.mExcluded, std::vector<Isin>{"XX0000000010"});

    // Rows in ledger order: the coupon of Bond B (15 March), then the cash interest (16 March).
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);
    EXPECT_EQ(std::get<BondCoupon>(run.mLast.mOtherReport.mRows.at(0).mKind).mIsin, "XX0000000028");
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 50'000);
    EXPECT_TRUE(
        std::holds_alternative<BrokerCashInterest>(run.mLast.mOtherReport.mRows.at(1).mKind));
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(1).mGrossInterest, 20'000);

    // The error stays visible and no longer blocks; the form is made.
    const auto& obr = run.result().report(ReportType::OtherInterest);

    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);
    EXPECT_FALSE(run.result().mDiagnostics.at(0).mBlocking);
    EXPECT_EQ(obr.mStatus, ReportStatus::Generated);
    EXPECT_EQ(obr.mFailureResolution, FailureResolution::ExcludeFailedInstruments);
    ASSERT_EQ(obr.mExcludedInvestments.size(), 1U);
    EXPECT_EQ(obr.mExcludedInvestments.at(0).mIsin, "XX0000000010");
    EXPECT_EQ(obr.mExcludedInvestments.at(0).mDiagnosticIds,
              std::vector<std::string>{"diagnostic-0001"});
    EXPECT_EQ(run.result().mStatus, OverallStatus::CompletedWithWarnings);
}

TEST(OtherInterestFailureTest, ExcludingTheIsinAlsoDropsItsOpenPayerChoice) {
    // Two reference rows disagree on the issuer of Bond A: a payer choice, which never blocks.
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer A,TAX-A,1 Main Street,FR,,,issuer,manual,\n"
                    "XX0000000010,Issuer A2,TAX-A2,3 Main Street,DE,,,wikidata,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 100'000, // USD 10.00, no USD rate
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    // The payer choice and the failure question are both open.
    ASSERT_EQ(run.result().mRequests.size(), 2U);

    const auto failureRequest = std::find_if(
        run.result().mRequests.begin(),
        run.result().mRequests.end(),
        [](const DecisionRequest& aRequest) {
            return aRequest.mType == RequestType::OtherInterestFailureResolutionRequired;
        });

    ASSERT_NE(failureRequest, run.result().mRequests.end());

    const auto rejections = run.mSession.answer({DecisionAnswer{
        .mRequestId = failureRequest->mId,
        .mRunRevision = run.result().mRunRevision,
        .mValue = kExclude,
    }});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_EQ(run.obrStatus(), ReportStatus::NoData);
}

TEST(OtherInterestFailureTest, ExcludingTheOnlyCouponLeavesNoDataAndNoXml) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 100'000, // USD 10.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(kExclude);

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
    EXPECT_EQ(run.obrStatus(), ReportStatus::NoData);
    EXPECT_FALSE(run.result().report(ReportType::OtherInterest).mXml.has_value());
    EXPECT_EQ(run.result().report(ReportType::OtherInterest).mExcludedInvestments.size(), 1U);
}

TEST(OtherInterestFailureTest, FailingTheReportKeepsTheErrorBlockingAndExcludesNothing) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 100'000, // USD 10.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
        {.mName = "Synthetic Bond B",
         .mIsin = "XX0000000028",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                                          .mSources = {source(3, "synthetic-int-2")}},
                            .mGrossAmount = 50'000, // EUR 5.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::EUR,
                            .mTaxCurrency = Currency::EUR}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(kFailReport);
    const auto& obr = run.result().report(ReportType::OtherInterest);

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.mExcluded.empty());
    EXPECT_EQ(obr.mStatus, ReportStatus::Failed);
    EXPECT_EQ(obr.mFailureResolution, FailureResolution::FailReport);
    EXPECT_TRUE(obr.mExcludedInvestments.empty());
    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);
    EXPECT_TRUE(run.result().mDiagnostics.at(0).mBlocking);

    // The coupon of Bond B stays in the model, but a failed form has no XML.
    EXPECT_FALSE(obr.mXml.has_value());
}

TEST(OtherInterestFailureTest, CancellingTheQuestionFailsDohObrWithoutAResolution) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 100'000, // USD 10.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(CancelAnswer{});
    const auto& obr = run.result().report(ReportType::OtherInterest);

    EXPECT_TRUE(rejections.empty());
    EXPECT_EQ(obr.mStatus, ReportStatus::Failed);
    EXPECT_EQ(obr.mFailureResolution, std::nullopt);
    EXPECT_TRUE(obr.mExcludedInvestments.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
}

TEST(OtherInterestFailureTest, AChangedListOfIsinsNeedsANewAnswer) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement first;
    MergedStatement second;

    addFile2025(rates);
    first.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 100'000, // USD 10.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
    };
    second.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 100'000, // USD 10.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
        {.mName = "Synthetic Bond B",
         .mIsin = "XX0000000028",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                                          .mSources = {source(3, "synthetic-int-2")}},
                            .mGrossAmount = 80'000, // USD 8.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
    };

    orderChronologically(first);
    orderChronologically(second);
    ObrRun run{first, catalogue, rates};

    static_cast<void>(run.answerFirst(kExclude));
    EXPECT_EQ(run.mExcluded, std::vector<Isin>{"XX0000000010"});

    // Bond B now fails too: the list [A, B] is not the answered list [A].
    run.start(second, catalogue, rates);

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_EQ(run.result().mRequests.at(0).mInvestments.size(), 2U);
    EXPECT_EQ(run.result().mRequests.at(0).mInvestments.at(0).mIsin, "XX0000000010");
    EXPECT_EQ(run.result().mRequests.at(0).mInvestments.at(1).mIsin, "XX0000000028");
    EXPECT_TRUE(run.mExcluded.empty());
    EXPECT_EQ(run.obrStatus(), ReportStatus::NeedsInput);
    EXPECT_FALSE(run.result().report(ReportType::OtherInterest).mFailureResolution.has_value());
}

TEST(OtherInterestFailureTest, ACashInterestErrorNamesItsBrokerAndAsksTheFailureQuestion) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 100'000, // USD 10.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(3, "synthetic-int-2")}},
                            .mGrossAmount = 30'000, // USD 3.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD,
                            .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    // Two errors: the coupon's has its ISIN, the cash interest's has none and names its broker. One
    // question lists both units: the bond and the broker's cash interest.
    ASSERT_EQ(run.mLast.mDiagnostics.size(), 2U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mCashInterestBroker, std::nullopt);
    EXPECT_EQ(run.mLast.mDiagnostics.at(1).mContext.mIsin, std::nullopt);
    EXPECT_EQ(run.mLast.mDiagnostics.at(1).mContext.mCashInterestBroker,
              std::optional<Broker>{Broker::TradeRepublic});
    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    ASSERT_EQ(request.mInvestments.size(), 1U);
    EXPECT_EQ(request.mInvestments.at(0).mIsin, "XX0000000010");
    ASSERT_EQ(request.mBrokers.size(), 1U);
    EXPECT_EQ(request.mBrokers.at(0).mBroker, Broker::TradeRepublic);
    EXPECT_EQ(run.obrStatus(), ReportStatus::NeedsInput);
}

TEST(OtherInterestFailureTest, ADepositAndACouponWithoutARateOnOneDayGiveTwoErrorsOfTheirOwnForm) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 100'000, // USD 10.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD,
                            .mCashHolder = CashHolder::Bank}}},
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(3, "synthetic-int-2")}},
                            .mGrossAmount = 100'000, // USD 10.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 2U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mAffectedReports,
              std::vector<ReportType>{ReportType::BankDepositInterest});
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mIsin, std::nullopt);
    EXPECT_EQ(run.mLast.mDiagnostics.at(1).mContext.mAffectedReports,
              std::vector<ReportType>{ReportType::OtherInterest});
    EXPECT_EQ(run.mLast.mDiagnostics.at(1).mContext.mIsin, std::optional<Isin>{"XX0000000010"});

    // Leaving Bond A out of Doh-Obr does not touch the deposit error of Doh-DHO.
    static_cast<void>(run.answerFirst(kExclude));

    EXPECT_EQ(run.obrStatus(), ReportStatus::NoData);
    EXPECT_EQ(run.result().report(ReportType::BankDepositInterest).mStatus, ReportStatus::Failed);
}

TEST(OtherInterestFailureTest, ACouponThatDoesNotFitAfterTheConversionBelongsToItsIsin) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    addRate(rates, "USD", "2025-03-14", 50'000'000, 2); // 1 EUR = 0.50000000 USD
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         // USD 500'000'000'000'000 (5e18 in 4 decimals) / 0.5 = 1e19, above the largest amount.
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 5'000'000'000'000'000'000,
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mType,
              RequestType::OtherInterestFailureResolutionRequired);
    EXPECT_EQ(run.result().mRequests.at(0).mInvestments.at(0).mIsin, "XX0000000010");
}

TEST(OtherInterestFailureTest, AForeignTaxWithoutARateIsAnErrorOfTheCouponsIsin) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates); // loaded, but without any USD rate
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 100'000, // EUR 10.00, needs no rate
                            .mTaxPaid = 15'000,      // USD 1.50, has no rate
                            .mCurrency = Currency::EUR,
                            .mTaxCurrency = Currency::USD}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    // One error for the USD tax, with the ISIN of the coupon; the coupon is no row.
    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mInvestments.at(0).mIsin, "XX0000000010");
}

TEST(OtherInterestFailureTest, ARateFromAnEarlierDayKeepsItsWarningAndMakesTheRow) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    addRate(rates, "USD", "2025-03-13", 108'800'000, 2); // 1 EUR = 1.0880 USD
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                          .mSources = {source(2, "synthetic-int-1")}},
                            .mGrossAmount = 100'000, // USD 10.00, no rate on 14 March
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    // The rate of the day before is used with a warning; USD 10.00 / 1.0880 = EUR 9.1912.
    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::OfficialRateFromNearbyDate);
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 91'912);
    EXPECT_TRUE(run.result().mRequests.empty());
}

// --- The bond type ---

TEST(OtherInterestFailureTest, ASoldBondAsksItsTypeAndHoldsBackItsCouponsWithoutAChoice) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mAssetClass = AssetClass::Bond,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 1000 * kOneUnit,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-02"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 400 * kOneUnit,
                 .mCurrency = Currency::EUR},
            },
    }};
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                         .mSources = {source(3, "synthetic-int-1")}},
                           .mGrossAmount = 50'000, // EUR 5.00
                           .mTaxPaid = 0,
                           .mCurrency = Currency::EUR,
                           .mTaxCurrency = Currency::EUR}},
    }};

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    EXPECT_EQ(request.mType, RequestType::IncomeDetailsRequired);
    EXPECT_EQ(request.mAffectedReports, std::vector<ReportType>{ReportType::OtherInterest});
    EXPECT_EQ(request.mIsin, std::optional<Isin>{"XX0000000010"});

    const auto& question = std::get<BondTypeQuestion>(request.mData);

    EXPECT_EQ(question.mIsin, "XX0000000010");
    EXPECT_EQ(question.mName, "Synthetic Bond A");
    EXPECT_EQ(question.mSoldUnits, 400 * kOneUnit);
    EXPECT_EQ(question.mMaturedUnits, 0);
    EXPECT_EQ(question.mMaturityDate, std::nullopt);

    // Until it is answered, the coupon is not a row, and Doh-Obr waits.
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
    EXPECT_EQ(run.obrStatus(), ReportStatus::NeedsInput);
}

TEST(OtherInterestFailureTest, AnOrdinaryBondKeepsItsCouponsAsBefore) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mAssetClass = AssetClass::Bond,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 1000 * kOneUnit,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-02"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 400 * kOneUnit,
                 .mCurrency = Currency::EUR},
            },
    }};
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                         .mSources = {source(3, "synthetic-int-1")}},
                           .mGrossAmount = 50'000, // EUR 5.00
                           .mTaxPaid = 0,
                           .mCurrency = Currency::EUR,
                           .mTaxCurrency = Currency::EUR}},
    }};

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(BondTypeAnswer{BondType::Ordinary});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 50'000);
    EXPECT_EQ(run.obrStatus(), ReportStatus::Generated);
}

TEST(OtherInterestFailureTest, ADiscountBondIsAnUnsupportedErrorThatTheFailureQuestionCanLeaveOut) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mAssetClass = AssetClass::Bond,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 1000 * kOneUnit,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-02"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 400 * kOneUnit,
                 .mCurrency = Currency::EUR},
            },
    }};
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                         .mSources = {source(3, "synthetic-int-1")}},
                           .mGrossAmount = 50'000, // EUR 5.00
                           .mTaxPaid = 0,
                           .mCurrency = Currency::EUR,
                           .mTaxCurrency = Currency::EUR}},
    }};

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    static_cast<void>(run.answerFirst(BondTypeAnswer{BondType::DiscountOrZeroCoupon}));

    // One blocking error of the bond; the coupon is no row.
    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);

    const auto& error = run.mLast.mDiagnostics.at(0);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, ProcessingDiagnosticCode::UnsupportedTaxTreatment);
    EXPECT_EQ(error.mMessageKey, MessageKey::UnsupportedTaxTreatmentDiscountBond);
    EXPECT_EQ(error.mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_EQ(error.mContext.mAffectedReports, std::vector<ReportType>{ReportType::OtherInterest});
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());

    // The failure question lists the bond; leaving it out ends in no data.
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mType,
              RequestType::OtherInterestFailureResolutionRequired);
    EXPECT_EQ(
        run.result().mRequests.at(0).mInvestments.at(0).mReasonCodes,
        std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::UnsupportedTaxTreatment});

    static_cast<void>(run.answerFirst(kExclude));

    EXPECT_EQ(run.obrStatus(), ReportStatus::NoData);
    EXPECT_EQ(run.result().report(ReportType::OtherInterest).mExcludedInvestments.size(), 1U);
}

TEST(OtherInterestFailureTest, CancellingTheBondTypeGivesTheNotGivenError) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mAssetClass = AssetClass::Bond,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 1000 * kOneUnit,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-02"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 400 * kOneUnit,
                 .mCurrency = Currency::EUR},
            },
    }};
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                         .mSources = {source(3, "synthetic-int-1")}},
                           .mGrossAmount = 50'000, // EUR 5.00
                           .mTaxPaid = 0,
                           .mCurrency = Currency::EUR,
                           .mTaxCurrency = Currency::EUR}},
    }};

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    static_cast<void>(run.answerFirst(CancelAnswer{}));

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::UnsupportedTaxTreatment);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mMessageKey,
              MessageKey::UnsupportedTaxTreatmentBondTypeNotGiven);
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
}

TEST(OtherInterestFailureTest, TheSessionTakesOnlyABondTypeOrACancelForTheBondQuestion) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mAssetClass = AssetClass::Bond,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-06-02"),
                                         .mSources = {source(4, "synthetic-sell-1")}},
                           .mTradeSide = TradeSide::Sell,
                           .mUnitPrice = 10'000'000'000, // EUR 100
                           .mUnits = 400 * kOneUnit,
                           .mCurrency = Currency::EUR}},
    }};

    ObrRun run{statement, catalogue, rates};

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_FALSE(run.answerFirst(CapitalGainsAnswer{}).empty());
    EXPECT_FALSE(run.answerFirst(PayerChoiceAnswer{.mPickedOption = std::nullopt}).empty());
    EXPECT_FALSE(run.answerFirst(kExclude).empty());
    EXPECT_EQ(run.result().mRequests.size(), 1U);
}

TEST(OtherInterestFailureTest, AMaturityAsksTheBondTypeWithTheUnitsAndTheDate) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mBondRedemptions = {
        {.mMetadata = {.mTaxDate = day("2025-09-30"), .mSources = {source(2, "synthetic-mat-1")}},
         .mKind = BondRedemptionKind::Maturity,
         .mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mUnits = 1000 * kOneUnit},
    };

    ObrRun run{statement, catalogue, rates};

    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& question = std::get<BondTypeQuestion>(run.result().mRequests.at(0).mData);

    EXPECT_EQ(question.mSoldUnits, 0);
    EXPECT_EQ(question.mMaturedUnits, 1000 * kOneUnit);
    EXPECT_EQ(question.mMaturityDate, std::optional<Date>{day("2025-09-30")});
    EXPECT_EQ(run.obrStatus(), ReportStatus::NeedsInput);
}

TEST(OtherInterestFailureTest, ACancelledMaturityAsksNothing) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    // 1000 units leave and 1000 come back: nothing matured.
    statement.mPresentation.mBondRedemptions = {
        {.mMetadata = {.mTaxDate = day("2025-09-30"), .mSources = {source(2, "synthetic-mat-1")}},
         .mKind = BondRedemptionKind::Maturity,
         .mName = "Synthetic Bond B",
         .mIsin = "XX0000000028",
         .mUnits = 1000 * kOneUnit},
        {.mMetadata = {.mTaxDate = day("2025-09-30"), .mSources = {source(3, "synthetic-mat-2")}},
         .mKind = BondRedemptionKind::MaturityCancelled,
         .mName = "Synthetic Bond B",
         .mIsin = "XX0000000028",
         .mUnits = 1000 * kOneUnit},
    };

    ObrRun run{statement, catalogue, rates};

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
}

TEST(OtherInterestFailureTest, ABondOnlyBoughtInTheYearAsksNothingAndKeepsItsCoupon) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mAssetClass = AssetClass::Bond,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-01-10"),
                                         .mSources = {source(2, "synthetic-buy-1")}},
                           .mTradeSide = TradeSide::Buy,
                           .mUnitPrice = 10'000'000'000, // EUR 100
                           .mUnits = 1000 * kOneUnit,
                           .mCurrency = Currency::EUR}},
    }};
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                         .mSources = {source(3, "synthetic-int-1")}},
                           .mGrossAmount = 50'000, // EUR 5.00
                           .mTaxPaid = 0,
                           .mCurrency = Currency::EUR,
                           .mTaxCurrency = Currency::EUR}},
    }};

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    EXPECT_TRUE(run.result().mRequests.empty());
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 50'000);
}

TEST(OtherInterestFailureTest, ABondSoldInAnotherYearAsksNothing) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mAssetClass = AssetClass::Bond,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2024-06-02"),
                                         .mSources = {source(2, "synthetic-sell-1")}},
                           .mTradeSide = TradeSide::Sell,
                           .mUnitPrice = 10'000'000'000, // EUR 100
                           .mUnits = 400 * kOneUnit,
                           .mCurrency = Currency::EUR}},
    }};

    ObrRun run{statement, catalogue, rates};

    EXPECT_TRUE(run.result().mRequests.empty());
}

TEST(OtherInterestFailureTest, ASoldBondOfASlovenianIssuerAsksTheBondTypeLikeAnyOther) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer SI,TAX-S,1 Main Street,SI,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mAssetClass = AssetClass::Bond,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-06-02"),
                                         .mSources = {source(4, "synthetic-sell-1")}},
                           .mTradeSide = TradeSide::Sell,
                           .mUnitPrice = 10'000'000'000, // EUR 100
                           .mUnits = 400 * kOneUnit,
                           .mCurrency = Currency::EUR}},
    }};
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                         .mSources = {source(3, "synthetic-int-1")}},
                           .mGrossAmount = 50'000, // EUR 5.00
                           .mTaxPaid = 0,
                           .mCurrency = Currency::EUR,
                           .mTaxCurrency = Currency::EUR}},
    }};

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    // The payer does not withhold on a listed Slovenian bond, so the bond type is asked and the
    // coupon waits for the answer. Nothing is left out as settled by a Slovenian payer.
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<BondTypeQuestion>(run.result().mRequests.at(0).mData));
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
    EXPECT_TRUE(run.result().mExcludedItems.empty());
    EXPECT_EQ(run.obrStatus(), ReportStatus::NeedsInput);
}

TEST(OtherInterestFailureTest, AnOrdinarySlovenianBondReportsItsCouponWithTheIssuersFacts) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer SI,TAX-S,1 Main Street,SI,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mAssetClass = AssetClass::Bond,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-06-02"),
                                         .mSources = {source(4, "synthetic-sell-1")}},
                           .mTradeSide = TradeSide::Sell,
                           .mUnitPrice = 10'000'000'000, // EUR 100
                           .mUnits = 400 * kOneUnit,
                           .mCurrency = Currency::EUR}},
    }};
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                         .mSources = {source(3, "synthetic-int-1")}},
                           .mGrossAmount = 50'000, // EUR 5.00
                           .mTaxPaid = 0,
                           .mCurrency = Currency::EUR,
                           .mTaxCurrency = Currency::EUR}},
    }};

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(BondTypeAnswer{BondType::Ordinary});

    // EUR 5.00 gross, with the facts of the Slovenian issuer.
    EXPECT_TRUE(rejections.empty());
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 50'000);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mPayer.mCountry, "SI");
    EXPECT_EQ(run.obrStatus(), ReportStatus::Generated);
}

TEST(OtherInterestFailureTest, AMaturedBondWithoutCouponsOfASlovenianIssuerAsksTheBondType) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer SI,TAX-S,1 Main Street,SI,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mBondRedemptions = {
        {.mMetadata = {.mTaxDate = day("2025-09-30"), .mSources = {source(2, "synthetic-mat-1")}},
         .mKind = BondRedemptionKind::Maturity,
         .mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mUnits = 1000 * kOneUnit},
    };

    ObrRun run{statement, catalogue, rates};

    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& question = std::get<BondTypeQuestion>(run.result().mRequests.at(0).mData);

    EXPECT_EQ(question.mMaturedUnits, 1000 * kOneUnit);
    EXPECT_EQ(question.mMaturityDate, std::optional<Date>{day("2025-09-30")});
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
}

TEST(OtherInterestFailureTest, IssuerRowsWithDifferentCountriesStillAskTheBondType) {
    // Two rows for one ISIN, one Slovenian and one French, and no coupon in the year.
    const auto catalogue =
        catalogueOf("XX0000000028,Issuer S,TAX-S,1 Main Street,SI,,,issuer,manual,\n"
                    "XX0000000028,Issuer F,TAX-F,2 Main Street,FR,,,wikidata,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mBondRedemptions = {
        {.mMetadata = {.mTaxDate = day("2025-09-30"), .mSources = {source(2, "synthetic-mat-1")}},
         .mKind = BondRedemptionKind::Maturity,
         .mName = "Synthetic Bond B",
         .mIsin = "XX0000000028",
         .mUnits = 1000 * kOneUnit},
    };

    ObrRun run{statement, catalogue, rates};

    // Only the bond type question: a bond without coupons asks no payer choice.
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<BondTypeQuestion>(run.result().mRequests.at(0).mData));
}

TEST(OtherInterestFailureTest, SeveralSlovenianIssuerRowsAskThePayerAndTheBondType) {
    // Two rows for one ISIN, both Slovenian: a coupon is reported, so the payer row is still asked.
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer S1,TAX-S1,1 Main Street,SI,,,issuer,manual,\n"
                    "XX0000000010,Issuer S2,TAX-S2,2 Main Street,SI,,,wikidata,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mAssetClass = AssetClass::Bond,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-06-02"),
                                         .mSources = {source(4, "synthetic-sell-1")}},
                           .mTradeSide = TradeSide::Sell,
                           .mUnitPrice = 10'000'000'000, // EUR 100
                           .mUnits = 400 * kOneUnit,
                           .mCurrency = Currency::EUR}},
    }};
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                         .mSources = {source(3, "synthetic-int-1")}},
                           .mGrossAmount = 50'000, // EUR 5.00
                           .mTaxPaid = 0,
                           .mCurrency = Currency::EUR,
                           .mTaxCurrency = Currency::EUR}},
    }};

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    // Two open questions: the payer choice and the bond type. Nothing is excluded as settled.
    ASSERT_EQ(run.result().mRequests.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<PayerChoiceQuestion>(run.result().mRequests.at(0).mData));
    EXPECT_TRUE(std::holds_alternative<BondTypeQuestion>(run.result().mRequests.at(1).mData));
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
    EXPECT_TRUE(run.result().mExcludedItems.empty());
}

TEST(OtherInterestFailureTest, DisagreeingIssuerRowsWithACouponStillAskThePayerAndTheBondType) {
    // One Slovenian and one French row: the payer row is asked.
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer S,TAX-S,1 Main Street,SI,,,issuer,manual,\n"
                    "XX0000000010,Issuer F,TAX-F,2 Main Street,FR,,,wikidata,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mAssetClass = AssetClass::Bond,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-06-02"),
                                         .mSources = {source(4, "synthetic-sell-1")}},
                           .mTradeSide = TradeSide::Sell,
                           .mUnitPrice = 10'000'000'000, // EUR 100
                           .mUnits = 400 * kOneUnit,
                           .mCurrency = Currency::EUR}},
    }};
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                         .mSources = {source(3, "synthetic-int-1")}},
                           .mGrossAmount = 50'000, // EUR 5.00
                           .mTaxPaid = 0,
                           .mCurrency = Currency::EUR,
                           .mTaxCurrency = Currency::EUR}},
    }};

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    // Two open questions: the payer choice and the bond type.
    ASSERT_EQ(run.result().mRequests.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<PayerChoiceQuestion>(run.result().mRequests.at(0).mData));
    EXPECT_TRUE(std::holds_alternative<BondTypeQuestion>(run.result().mRequests.at(1).mData));
}

TEST(OtherInterestFailureTest, ABondWhoseUnitsOverflowIsAnErrorOfItsIsinAndHoldsBackItsCoupons) {
    const auto catalogue = catalogueOf(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    // 9'223'372'036'854'775'807 (the largest unit total) plus 1 unit does not fit.
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mAssetClass = AssetClass::Bond,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = std::numeric_limits<Units>::max(),
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-11"),
                               .mSources = {source(3, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR},
            },
    }};
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                                         .mSources = {source(4, "synthetic-int-1")}},
                           .mGrossAmount = 50'000, // EUR 5.00
                           .mTaxPaid = 0,
                           .mCurrency = Currency::EUR,
                           .mTaxCurrency = Currency::EUR}},
    }};

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    // One blocking overflow error of the bond, no bond type question, no coupon row.
    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);

    const auto& error = run.mLast.mDiagnostics.at(0);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, ProcessingDiagnosticCode::ArithmeticOverflow);
    EXPECT_EQ(error.mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_EQ(error.mContext.mAffectedReports, std::vector<ReportType>{ReportType::OtherInterest});
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());

    // The failure question lists the bond.
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mType,
              RequestType::OtherInterestFailureResolutionRequired);
    EXPECT_EQ(run.result().mRequests.at(0).mInvestments.at(0).mIsin, "XX0000000010");
}

TEST(OtherInterestFailureTest, ExcludingCouponsKeepsCashInterestAndTheOtherIsins) {
    OtherInterestReportObr report;

    report.mRows = {
        ObrInterestRow{.mKind = BondCoupon{.mIsin = "XX0000000010"}, .mGrossInterest = 10'000},
        ObrInterestRow{.mKind = BrokerCashInterest{}, .mGrossInterest = 20'000},
        ObrInterestRow{.mKind = BondCoupon{.mIsin = "XX0000000028"}, .mGrossInterest = 30'000},
        ObrInterestRow{.mKind = BondCoupon{.mIsin = "XX0000000010"}, .mGrossInterest = 40'000},
    };

    excludeCoupons(report, {"XX0000000010"});

    ASSERT_EQ(report.mRows.size(), 2U);
    EXPECT_EQ(report.mRows.at(0).mGrossInterest, 20'000);
    EXPECT_EQ(report.mRows.at(1).mGrossInterest, 30'000);
}

// --- A cash-interest error of one broker ---

TEST(OtherInterestFailureTest,
     ABrokersCashInterestCanBeLeftOutWhileTheOtherBrokerAndTheCouponStay) {
    const auto catalogue = catalogueOfTwoBrokers(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates); // loaded, but without any USD rate
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-13"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 50'000, // EUR 5.00
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 30'000, // USD 3.00
               .mTaxPaid = 0,
               .mCurrency = Currency::USD,
               .mTaxCurrency = Currency::USD,
               .mCashHolder = CashHolder::Broker}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                                          .mSources = {sourceOf(Broker::InteractiveBrokers,
                                                                4,
                                                                "synthetic-int-3")}},
                            .mGrossAmount = 40'000, // EUR 4.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::EUR,
                            .mTaxCurrency = Currency::EUR,
                            .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    // The cash interest of Trade Republic has no USD rate: one error, with its broker and no ISIN.
    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mIsin, std::nullopt);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mCashInterestBroker,
              std::optional<Broker>{Broker::TradeRepublic});

    // The question lists the broker only; Doh-Obr waits for the answer.
    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    EXPECT_EQ(request.mType, RequestType::OtherInterestFailureResolutionRequired);
    EXPECT_TRUE(request.mInvestments.empty());
    ASSERT_EQ(request.mBrokers.size(), 1U);
    EXPECT_EQ(request.mBrokers.at(0).mBroker, Broker::TradeRepublic);
    EXPECT_EQ(
        request.mBrokers.at(0).mReasonCodes,
        std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::MissingExchangeRate});
    EXPECT_EQ(request.mBrokers.at(0).mDiagnosticIds, std::vector<std::string>{"diagnostic-0001"});
    EXPECT_EQ(run.obrStatus(), ReportStatus::NeedsInput);

    EXPECT_TRUE(run.answerFirst(kExclude).empty());

    // The rows of the coupon (EUR 5.00) and of the other broker (EUR 4.00) stay.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 50'000);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(1).mGrossInterest, 40'000);

    // The error stays visible and no longer blocks; the form is made.
    const auto& obr = run.result().report(ReportType::OtherInterest);

    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);
    EXPECT_FALSE(run.result().mDiagnostics.at(0).mBlocking);
    EXPECT_EQ(obr.mStatus, ReportStatus::Generated);
    EXPECT_EQ(obr.mFailureResolution, FailureResolution::ExcludeFailedInstruments);
    EXPECT_TRUE(obr.mExcludedInvestments.empty());
    ASSERT_EQ(obr.mExcludedBrokers.size(), 1U);
    EXPECT_EQ(obr.mExcludedBrokers.at(0).mBroker, Broker::TradeRepublic);
    EXPECT_EQ(obr.mExcludedBrokers.at(0).mDiagnosticIds,
              std::vector<std::string>{"diagnostic-0001"});
}

TEST(OtherInterestFailureTest, ABrokerAndACouponThatFailTogetherAreOneQuestionAndBothAreLeftOut) {
    const auto catalogue = catalogueOfTwoBrokers(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates); // loaded, but without any USD rate
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-13"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 100'000, // USD 10.00
               .mTaxPaid = 0,
               .mCurrency = Currency::USD,
               .mTaxCurrency = Currency::USD}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 30'000, // USD 3.00
               .mTaxPaid = 0,
               .mCurrency = Currency::USD,
               .mTaxCurrency = Currency::USD,
               .mCashHolder = CashHolder::Broker}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                                          .mSources = {sourceOf(Broker::InteractiveBrokers,
                                                                4,
                                                                "synthetic-int-3")}},
                            .mGrossAmount = 40'000, // EUR 4.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::EUR,
                            .mTaxCurrency = Currency::EUR,
                            .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 2U);
    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    ASSERT_EQ(request.mInvestments.size(), 1U);
    EXPECT_EQ(request.mInvestments.at(0).mIsin, "XX0000000010");
    ASSERT_EQ(request.mBrokers.size(), 1U);
    EXPECT_EQ(request.mBrokers.at(0).mBroker, Broker::TradeRepublic);

    EXPECT_TRUE(run.answerFirst(kExclude).empty());

    // Only the cash interest of the other broker (EUR 4.00) is left.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 40'000);

    const auto& obr = run.result().report(ReportType::OtherInterest);

    EXPECT_EQ(obr.mStatus, ReportStatus::Generated);
    ASSERT_EQ(obr.mExcludedInvestments.size(), 1U);
    EXPECT_EQ(obr.mExcludedInvestments.at(0).mDiagnosticIds,
              std::vector<std::string>{"diagnostic-0001"});
    ASSERT_EQ(obr.mExcludedBrokers.size(), 1U);
    EXPECT_EQ(obr.mExcludedBrokers.at(0).mDiagnosticIds,
              std::vector<std::string>{"diagnostic-0002"});
}

TEST(OtherInterestFailureTest, FailingDohObrBecauseOfABrokerWritesNoFileAndExcludesNothing) {
    const auto catalogue = catalogueOfTwoBrokers(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates); // loaded, but without any USD rate
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-13"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 50'000, // EUR 5.00
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 30'000, // USD 3.00
               .mTaxPaid = 0,
               .mCurrency = Currency::USD,
               .mTaxCurrency = Currency::USD,
               .mCashHolder = CashHolder::Broker}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                                          .mSources = {sourceOf(Broker::InteractiveBrokers,
                                                                4,
                                                                "synthetic-int-3")}},
                            .mGrossAmount = 40'000, // EUR 4.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::EUR,
                            .mTaxCurrency = Currency::EUR,
                            .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    EXPECT_TRUE(run.answerFirst(kFailReport).empty());

    const auto& obr = run.result().report(ReportType::OtherInterest);

    EXPECT_EQ(obr.mStatus, ReportStatus::Failed);
    EXPECT_EQ(obr.mFailureResolution, FailureResolution::FailReport);
    EXPECT_TRUE(obr.mExcludedInvestments.empty());
    EXPECT_TRUE(obr.mExcludedBrokers.empty());
    EXPECT_FALSE(obr.mXml.has_value());
}

TEST(OtherInterestFailureTest, CancellingTheBrokerQuestionFailsDohObrWithoutAnAnswerOnRecord) {
    const auto catalogue = catalogueOfTwoBrokers(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates); // loaded, but without any USD rate
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-13"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 50'000, // EUR 5.00
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 30'000, // USD 3.00
               .mTaxPaid = 0,
               .mCurrency = Currency::USD,
               .mTaxCurrency = Currency::USD,
               .mCashHolder = CashHolder::Broker}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                                          .mSources = {sourceOf(Broker::InteractiveBrokers,
                                                                4,
                                                                "synthetic-int-3")}},
                            .mGrossAmount = 40'000, // EUR 4.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::EUR,
                            .mTaxCurrency = Currency::EUR,
                            .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    EXPECT_TRUE(run.answerFirst(CancelAnswer{}).empty());

    const auto& obr = run.result().report(ReportType::OtherInterest);

    EXPECT_EQ(obr.mStatus, ReportStatus::Failed);
    EXPECT_FALSE(obr.mFailureResolution.has_value());
    EXPECT_TRUE(obr.mExcludedBrokers.empty());
}

TEST(OtherInterestFailureTest, ARecordedBrokerExclusionDoesNotCoverAnotherBrokerThatFailsLater) {
    const auto catalogue = catalogueOfTwoBrokers(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement first;
    MergedStatement second;

    addFile2025(rates); // loaded, but without any USD rate
    first.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-13"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 50'000, // EUR 5.00
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 30'000, // USD 3.00
               .mTaxPaid = 0,
               .mCurrency = Currency::USD,
               .mTaxCurrency = Currency::USD,
               .mCashHolder = CashHolder::Broker}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                                          .mSources = {sourceOf(Broker::InteractiveBrokers,
                                                                4,
                                                                "synthetic-int-3")}},
                            .mGrossAmount = 40'000, // EUR 4.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::EUR,
                            .mTaxCurrency = Currency::EUR,
                            .mCashHolder = CashHolder::Broker}}},
    };
    second.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-13"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 50'000, // EUR 5.00
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 30'000, // USD 3.00
               .mTaxPaid = 0,
               .mCurrency = Currency::USD,
               .mTaxCurrency = Currency::USD,
               .mCashHolder = CashHolder::Broker}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                                          .mSources = {sourceOf(Broker::InteractiveBrokers,
                                                                4,
                                                                "synthetic-int-3")}},
                            .mGrossAmount = 40'000, // USD 4.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::USD,
                            .mTaxCurrency = Currency::USD,
                            .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(first);
    orderChronologically(second);
    ObrRun run{first, catalogue, rates};

    EXPECT_TRUE(run.answerFirst(kExclude).empty());
    EXPECT_EQ(run.obrStatus(), ReportStatus::Generated);

    // The next run fails the cash interest of the other broker as well: the recorded answer was for
    // the list [Trade Republic] only, so the question is asked again with both brokers.
    run.start(second, catalogue, rates);

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_EQ(run.result().mRequests.at(0).mBrokers.size(), 2U);
    EXPECT_EQ(run.result().mRequests.at(0).mBrokers.at(0).mBroker, Broker::TradeRepublic);
    EXPECT_EQ(run.result().mRequests.at(0).mBrokers.at(1).mBroker, Broker::InteractiveBrokers);
    EXPECT_EQ(run.obrStatus(), ReportStatus::NeedsInput);
}

TEST(OtherInterestFailureTest, AnErrorNoUnitOwnsStopsTheBrokerQuestion) {
    // A cash-interest error of Trade Republic, and an error of the form without an ISIN or a
    // broker (a file-level error): leaving the broker out would not fix Doh-Obr, so nothing is
    // asked.
    DecisionSession session;
    std::vector<Broker> excludedBrokers;

    session.start([&excludedBrokers](RunDecisions& aDecisions) {
        ApplicationResultInput input{.mReportingYear = 2025};

        input.mProcessingDiagnostics.push_back(ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
            .mContext = {.mStage = PipelineStage::FifoTax,
                         .mAffectedReports = {ReportType::OtherInterest},
                         .mCashInterestBroker = Broker::TradeRepublic},
        });
        input.mProcessingDiagnostics.push_back(ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
            .mContext = {.mStage = PipelineStage::FifoTax,
                         .mAffectedReports = {ReportType::OtherInterest}},
        });
        excludedBrokers =
            FailureResolutionStage::resolveUnits(input, aDecisions, kOtherInterestFailure).mBrokers;

        return input;
    });

    EXPECT_TRUE(session.result().mRequests.empty());
    EXPECT_TRUE(excludedBrokers.empty());
    EXPECT_EQ(session.result().report(ReportType::OtherInterest).mStatus, ReportStatus::Failed);
}

TEST(OtherInterestFailureTest, ADepositErrorNamesNoBrokerAndAsksNoQuestion) {
    // The deposit interest of a bank has no rate: Doh-DHO fails. Leaving a bank out would change
    // the shared EUR 1,000 total, so there is no broker unit and no question.
    const auto catalogue = catalogueOfTwoBrokers(kFrenchIssuers);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates); // loaded, but without any USD rate
    statement.mPresentation.mInterestInstruments = {
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {sourceOf(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 30'000, // USD 3.00
               .mTaxPaid = 0,
               .mCurrency = Currency::USD,
               .mTaxCurrency = Currency::USD,
               .mCashHolder = CashHolder::Bank}}},
    };

    orderChronologically(statement);
    ObrRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mAffectedReports,
              std::vector<ReportType>{ReportType::BankDepositInterest});
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mCashInterestBroker, std::nullopt);
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_EQ(run.result().report(ReportType::BankDepositInterest).mStatus, ReportStatus::Failed);
}

TEST(OtherInterestFailureTest, ExcludingBrokersKeepsCouponsAndTheCashInterestOfOtherBrokers) {
    OtherInterestReportObr report;

    report.mRows = {
        ObrInterestRow{
            .mPayment = {StatementEventKind::Interest, std::nullopt, 0},
            .mKind = BrokerCashInterest{.mBroker = Broker::TradeRepublic},
            .mDate = day("2025-01-31"),
            .mPayer = {.mName = "Example Bank A", .mCountry = "DE"},
            .mGrossInterest = 10'000, // EUR 1.00
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
            .mSourceCountry = "DE",
            .mTreatyClaim = std::nullopt,
        },
        ObrInterestRow{
            .mPayment = {StatementEventKind::Interest, 0, 1},
            .mKind = BondCoupon{.mIsin = "XX0000000010"},
            .mDate = day("2025-02-28"),
            .mPayer = {.mName = "Issuer A", .mCountry = "FR"},
            .mGrossInterest = 20'000, // EUR 2.00
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
            .mSourceCountry = "FR",
            .mTreatyClaim = std::nullopt,
        },
        ObrInterestRow{
            .mPayment = {StatementEventKind::Interest, std::nullopt, 2},
            .mKind = BrokerCashInterest{.mBroker = Broker::InteractiveBrokers},
            .mDate = day("2025-03-31"),
            .mPayer = {.mName = "Example Bank B", .mCountry = "IE"},
            .mGrossInterest = 30'000, // EUR 3.00
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
            .mSourceCountry = "IE",
            .mTreatyClaim = std::nullopt,
        },
        ObrInterestRow{
            .mPayment = {StatementEventKind::Interest, std::nullopt, 3},
            .mKind = BrokerCashInterest{.mBroker = Broker::TradeRepublic},
            .mDate = day("2025-04-30"),
            .mPayer = {.mName = "Example Bank A", .mCountry = "DE"},
            .mGrossInterest = 40'000, // EUR 4.00
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
            .mSourceCountry = "DE",
            .mTreatyClaim = std::nullopt,
        },
    };

    excludeBrokerCashInterest(report, {Broker::TradeRepublic});

    // Both cash rows of the first broker are gone; the coupon and the other broker's row stay.
    ASSERT_EQ(report.mRows.size(), 2U);
    EXPECT_EQ(report.mRows.at(0).mGrossInterest, 20'000);
    EXPECT_EQ(report.mRows.at(1).mGrossInterest, 30'000);
}

} // namespace
