#include "processors/interest_payments.hpp"

#include "taxbroker/currency.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

// All payers, ISINs, dates, amounts and rates in this test suite are synthetic. The synthetic
// ISINs use the prefix XX and a valid check digit (XX0000000010, XX0000000028). Money is stored
// with 4 decimals, so 300'000 is EUR 30.00 and 3'000'000 is EUR 300.00.

constexpr std::string_view kPayersHeader =
    "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
    "source_revision\n";
constexpr std::string_view kReliefHeader = "country,income,statement,source,source_revision\n";
constexpr std::string_view kBrokerHeader =
    "broker,code,name,tax_number,address,country,source,source_revision\n";

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

SourceReference source(Broker aBroker, std::size_t aRow, std::string aTransactionId) {
    return SourceReference{
        .mBroker = aBroker,
        .mFilename = SourceFilename::fromPath("history.csv"),
        .mSourceRow = aRow,
        .mTransactionId = std::move(aTransactionId),
    };
}

ReportingYearLedger ledgerOf(int aYear) {
    const auto year = std::to_string(aYear);

    return ReportingYearLedger{.mYear = aYear,
                               .mYearStart = day(year + "-01-01"),
                               .mYearEnd = day(year + "-12-31")};
}

/// A catalogue with the given payer rows, one broker payer for Trade Republic and two for
/// Interactive Brokers (so that no single company is known for it).
ReferenceCatalogue catalogueOf(std::string_view aPayerRows) {
    const std::string payers = std::string{kPayersHeader} + std::string{aPayerRows};
    const std::string relief = std::string{kReliefHeader} + "BE,dividend,Treaty text,manual,\n";
    const std::string broker = std::string{kBrokerHeader} +
                               "TR,TR-X,Broker T,TAX-T,\"1 Main Street, Berlin\",DE,manual,\n"
                               "IBKR,IB-X,Broker X,123,1 Side Street,GB,manual,\n"
                               "IBKR,IB-Y,Broker Y,456,2 Side Street,IE,manual,\n";

    return parseReferenceCatalogue(payers, relief, broker);
}

/// Marks the rate files of 2025 as loaded, from 2 January to 31 December.
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

/// Runs the stage in a session, so a test can answer its questions.
struct InterestRun {
    DecisionSession mSession;
    InterestPaymentsResult mLast;

    InterestRun(const MergedStatement& aStatement,
                const ReferenceCatalogue& aCatalogue,
                const OfficialRateTable& aRates,
                int aYear = 2025) {
        start(aStatement, aCatalogue, aRates, aYear);
    }

    /// A new run with new input; the recorded answers stay.
    void start(const MergedStatement& aStatement,
               const ReferenceCatalogue& aCatalogue,
               const OfficialRateTable& aRates,
               int aYear = 2025) {
        mSession.start([this, &aStatement, &aCatalogue, &aRates, aYear](RunDecisions& aDecisions) {
            mLast = prepareInterestPayments(aStatement,
                                            ledgerOf(aYear),
                                            aCatalogue,
                                            aRates,
                                            aDecisions);

            ApplicationResultInput input{.mReportingYear = aYear};
            auto copy = mLast;

            appendInterestPayments(input, std::move(copy));

            return input;
        });
    }

    [[nodiscard]] const ApplicationResult& result() const {
        return mSession.result();
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

ReportStatus statusOf(const ApplicationResult& aResult, ReportType aReport) {
    return aResult.mReports.at(static_cast<std::size_t>(aReport)).mStatus;
}

std::size_t countOf(const std::vector<ProcessingDiagnostic>& aDiagnostics,
                    ProcessingDiagnosticCode aCode) {
    return static_cast<std::size_t>(
        std::count_if(aDiagnostics.begin(), aDiagnostics.end(), [aCode](const auto& aDiagnostic) {
            return aDiagnostic.mCode == aCode;
        }));
}

// --- The deposit allowance ---

TEST(InterestPaymentsTest, DepositInterestBelowTheAllowanceAsksAndMakesTheRowWithoutAnAnswer) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-28"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 3'000'000, // EUR 300.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
                {.mMetadata = {.mTaxDate = day("2025-03-31"),
                               .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                 .mGrossAmount = 3'000'000, // EUR 300.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
                {.mMetadata = {.mTaxDate = day("2025-04-30"),
                               .mSources = {source(Broker::TradeRepublic, 4, "synthetic-int-3")}},
                 .mGrossAmount = 3'000'000, // EUR 300.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // 3 x EUR 300.00 = EUR 900.00 is at most EUR 1,000: one question, and without an answer the
    // row is made as for yes, with the reminder to add the other banks.
    EXPECT_EQ(run.mLast.mQualifyingTotal, std::optional<Money>{9'000'000});
    ASSERT_EQ(run.mLast.mDepositReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(0).mInterest, 9'000'000);
    EXPECT_EQ(run.mLast.mDepositNoDataReason, std::nullopt);
    EXPECT_EQ(
        countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::DepositInterestAddOtherBanks),
        1U);
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    EXPECT_EQ(request.mType, RequestType::DepositInterestTotalRequired);
    EXPECT_EQ(request.mAffectedReports, std::vector<ReportType>{ReportType::BankDepositInterest});

    const auto* question = std::get_if<DepositInterestTotalQuestion>(&request.mData);

    ASSERT_NE(question, nullptr);
    EXPECT_EQ(question->mYear, 2025);
    EXPECT_EQ(question->mTotal, 9'000'000);

    // The open question is listed but stops no form. This test writes no XML, so Doh-DHO has no
    // data here; with XML it is generated (the CLI tests).
    EXPECT_EQ(statusOf(run.result(), ReportType::BankDepositInterest), ReportStatus::NoData);
    EXPECT_EQ(statusOf(run.result(), ReportType::OtherInterest), ReportStatus::NoData);
}

TEST(InterestPaymentsTest, AnswerYesMakesTheDepositRowFromTheImportedInterestWithAReminder) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-28"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 4'500'000, // EUR 450.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
                {.mMetadata = {.mTaxDate = day("2025-03-31"),
                               .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                 .mGrossAmount = 4'500'000, // EUR 450.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(DepositInterestTotalAnswer{.mAboveLimit = true});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_EQ(run.mLast.mDepositNoDataReason, std::nullopt);
    ASSERT_EQ(run.mLast.mDepositReport.mRows.size(), 1U);

    // The row holds all EUR 900.00 that was imported, not only an excess; the user adds the other
    // banks in eDavki.
    const auto& row = run.mLast.mDepositReport.mRows.at(0);

    EXPECT_EQ(row.mInterest, 9'000'000);
    EXPECT_EQ(row.mForeignTax, 0);
    EXPECT_EQ(row.mKind, DepositInterestKind::BankDeposit);
    ASSERT_EQ(row.mPayments.size(), 2U);
    EXPECT_EQ(row.mPayments.at(0).mEventIndex, 0U);
    EXPECT_EQ(row.mPayments.at(1).mEventIndex, 1U);
    EXPECT_EQ(row.mPayer.mName, std::optional<std::string>{"Broker T"});
    EXPECT_EQ(row.mPayer.mAddress, std::optional<std::string>{"1 Main Street, Berlin"});
    EXPECT_EQ(row.mPayer.mCountry, std::optional<std::string>{"DE"});
    EXPECT_EQ(row.mPayer.mIdentificationNumber, std::optional<std::string>{"TAX-T"});
    EXPECT_EQ(row.mSourceCountry, std::optional<std::string>{"DE"});
    EXPECT_EQ(
        countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::DepositInterestAddOtherBanks),
        1U);
    ASSERT_EQ(run.result().mDecisions.size(), 1U);
    EXPECT_TRUE(run.result().mDecisions.at(0).mUsed);
}

TEST(InterestPaymentsTest, AnswerNoGivesNoRowsAndTheBelowThresholdReason) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-28"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 9'000'000, // EUR 900.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(DepositInterestTotalAnswer{.mAboveLimit = false});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.mLast.mDepositReport.mRows.empty());
    EXPECT_EQ(run.mLast.mDepositNoDataReason,
              std::optional<NoDataReason>{NoDataReason::BelowDepositInterestThreshold});
    EXPECT_EQ(
        countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::DepositInterestAddOtherBanks),
        0U);
    EXPECT_EQ(statusOf(run.result(), ReportType::BankDepositInterest), ReportStatus::NoData);
}

TEST(InterestPaymentsTest, ExactlyTheAllowanceStillAsksTheQuestionAndMakesTheRow) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-28"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 5'000'000, // EUR 500.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
                {.mMetadata = {.mTaxDate = day("2025-03-31"),
                               .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                 .mGrossAmount = 5'000'000, // EUR 500.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // EUR 1,000.0000 is not above the allowance: the question is asked, and without an answer the
    // row is made from the imported interest.
    EXPECT_EQ(run.mLast.mQualifyingTotal, std::optional<Money>{10'000'000});
    ASSERT_EQ(run.mLast.mDepositReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(0).mInterest, 10'000'000);
    ASSERT_EQ(run.result().mRequests.size(), 1U);
}

TEST(InterestPaymentsTest, OneUnitOfTheFourthDecimalAboveTheAllowanceMakesTheRowWithoutAQuestion) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-28"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 5'000'000, // EUR 500.0000
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
                {.mMetadata = {.mTaxDate = day("2025-03-31"),
                               .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                 .mGrossAmount = 5'000'001, // EUR 500.0001
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // EUR 1,000.0001 is above the allowance, although it would be written as 1000.00.
    EXPECT_EQ(run.mLast.mQualifyingTotal, std::optional<Money>{10'000'001});
    EXPECT_TRUE(run.mLast.mRequests.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    ASSERT_EQ(run.mLast.mDepositReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(0).mInterest, 10'000'001);
    EXPECT_EQ(
        countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::DepositInterestAddOtherBanks),
        0U);
}

TEST(InterestPaymentsTest, AnAnswerForOneTotalDoesNotAnswerAnotherTotal) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-28"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 9'000'000, // EUR 900.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_TRUE(run.answerFirst(DepositInterestTotalAnswer{.mAboveLimit = true}).empty());
    ASSERT_EQ(run.mLast.mDepositReport.mRows.size(), 1U);

    // The next run has EUR 900.01: a different question that is not answered.
    MergedStatement changed = statement;

    changed.mPresentation.mInterestInstruments.at(0).mTransactions.at(0).mGrossAmount = 9'000'100;
    run.start(changed, catalogue, rates);

    ASSERT_EQ(run.mLast.mDepositReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(0).mInterest, 9'000'100);
    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto* question =
        std::get_if<DepositInterestTotalQuestion>(&run.result().mRequests.at(0).mData);

    ASSERT_NE(question, nullptr);
    EXPECT_EQ(question->mTotal, 9'000'100);
}

TEST(InterestPaymentsTest, TheSessionTakesOnlyYesOrNoForTheDepositQuestion) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-28"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 9'000'000, // EUR 900.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(CancelAnswer{});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::AnswerDoesNotFit);
    EXPECT_EQ(run.result().mRequests.size(), 1U);
}

// --- Routing ---

TEST(InterestPaymentsTest, TheCashHolderOfEachPaymentDecidesBetweenDohObrAndDohDho) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-12-05"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 100'000, // EUR 10.00, the broker holds the cash
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Broker},
                {.mMetadata = {.mTaxDate = day("2023-12-06"),
                               .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                 .mGrossAmount = 200'000, // EUR 20.00, the bank holds the cash
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
            },
    }};

    orderChronologically(statement);
    InterestRun run{statement, catalogue, rates, 2023};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mDate, day("2023-12-05"));
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 100'000);
    EXPECT_TRUE(
        std::holds_alternative<BrokerCashInterest>(run.mLast.mOtherReport.mRows.at(0).mKind));

    // Only the EUR 20.00 paid by the bank counts for the allowance.
    EXPECT_EQ(run.mLast.mQualifyingTotal, std::optional<Money>{200'000});
    ASSERT_EQ(run.result().mRequests.size(), 1U);
}

TEST(InterestPaymentsTest, ACashInterestPaymentWithoutACashHolderIsAProgrammingError) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-28"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 100'000, // EUR 10.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = std::nullopt},
            },
    }};

    orderChronologically(statement);

    // The parser must set the holder; a missing one is never guessed.
    EXPECT_THROW((InterestRun{statement, catalogue, rates}), std::logic_error);
}

TEST(InterestPaymentsTest, OnlyPaymentsOfTheReportingYearCount) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments =
        {
            {
                .mName = "",
                .mIsin = std::nullopt,
                .mInterestType = InterestType::BrokerInterest,
                .mTransactions =
                    {
                        {.mMetadata =
                             {.mTaxDate = day("2024-12-31"),
                              .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                         .mGrossAmount = 10'000, // EUR 1.00
                         .mTaxPaid = 0,
                         .mCurrency = Currency::EUR,
                         .mTaxCurrency = Currency::EUR,
                         .mCashHolder = CashHolder::Bank},
                        {.mMetadata =
                             {.mTaxDate = day("2025-01-01"),
                              .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                         .mGrossAmount = 20'000, // EUR 2.00
                         .mTaxPaid = 0,
                         .mCurrency = Currency::EUR,
                         .mTaxCurrency = Currency::EUR,
                         .mCashHolder = CashHolder::Bank},
                        {.mMetadata =
                             {.mTaxDate = day("2025-12-31"),
                              .mSources = {source(Broker::TradeRepublic, 4, "synthetic-int-3")}},
                         .mGrossAmount = 40'000, // EUR 4.00
                         .mTaxPaid = 0,
                         .mCurrency = Currency::EUR,
                         .mTaxCurrency = Currency::EUR,
                         .mCashHolder = CashHolder::Bank},
                        {.mMetadata =
                             {.mTaxDate = day("2026-01-01"),
                              .mSources = {source(Broker::TradeRepublic, 5, "synthetic-int-4")}},
                         .mGrossAmount = 80'000, // EUR 8.00
                         .mTaxPaid = 0,
                         .mCurrency = Currency::EUR,
                         .mTaxCurrency = Currency::EUR,
                         .mCashHolder = CashHolder::Bank},
                    },
            }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // EUR 2.00 + EUR 4.00 only: the receipt date decides, 1 January is in, the next one is out.
    EXPECT_EQ(run.mLast.mQualifyingTotal, std::optional<Money>{60'000});
}

TEST(InterestPaymentsTest, NoDepositInterestAsksNothingAndSetsNoReason) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer A,TAX-A,1 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-06-15"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    EXPECT_EQ(run.mLast.mQualifyingTotal, std::nullopt);
    EXPECT_EQ(run.mLast.mDepositNoDataReason, std::nullopt);
    EXPECT_TRUE(run.mLast.mDepositReport.mRows.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
}

TEST(InterestPaymentsTest, DepositInterestAboveTheAllowanceAndOtherInterestAreSeparateForms) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer A,TAX-A,1 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {
        {
            .mName = "Synthetic Bond A",
            .mIsin = "XX0000000010",
            .mInterestType = InterestType::BondInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-06-15"),
                          .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                     .mGrossAmount = 250'000, // EUR 25.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR},
                },
        },
        {
            .mName = "",
            .mIsin = std::nullopt,
            .mInterestType = InterestType::BrokerInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-02-28"),
                          .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                     .mGrossAmount = 12'000'000, // EUR 1,200.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR,
                     .mCashHolder = CashHolder::Bank},
                },
        },
        {
            .mName = "Interactive Brokers",
            .mIsin = std::nullopt,
            .mInterestType = InterestType::BrokerInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-07-01"),
                          .mSources = {source(Broker::InteractiveBrokers, 2, "synthetic-ib-1")}},
                     .mGrossAmount = 200'000, // EUR 20.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR,
                     .mCashHolder = CashHolder::Broker},
                },
        },
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // EUR 1,200.00 of Trade Republic deposit interest is above the allowance: Doh-DHO, no question.
    EXPECT_TRUE(run.result().mRequests.empty());
    ASSERT_EQ(run.mLast.mDepositReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(0).mInterest, 12'000'000);

    // The coupon (25 June) and the cash interest of IBKR (1 July) are Doh-Obr rows in date order.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);

    const auto& coupon = run.mLast.mOtherReport.mRows.at(0);

    ASSERT_TRUE(std::holds_alternative<BondCoupon>(coupon.mKind));
    EXPECT_EQ(std::get<BondCoupon>(coupon.mKind).mIsin, "XX0000000010");
    EXPECT_EQ(coupon.mGrossInterest, 250'000);
    EXPECT_EQ(coupon.mPayer.mName, std::optional<std::string>{"Issuer A"});
    EXPECT_EQ(coupon.mSourceCountry, std::optional<std::string>{"FR"});

    // Interactive Brokers has two companies in the file, so none is known for its cash interest:
    // empty facts and one warning for Doh-Obr.
    const auto& cash = run.mLast.mOtherReport.mRows.at(1);

    EXPECT_TRUE(std::holds_alternative<BrokerCashInterest>(cash.mKind));
    EXPECT_EQ(cash.mGrossInterest, 200'000);
    EXPECT_EQ(cash.mPayer, IncomePayer{});
    EXPECT_EQ(cash.mSourceCountry, std::nullopt);
    ASSERT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::InterestPayerNotFound), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mAffectedReports,
              std::vector<ReportType>{ReportType::OtherInterest});
}

TEST(InterestPaymentsTest, CashInterestOfAnotherBrokerDoesNotCountForTheAllowance) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {
        {
            .mName = "",
            .mIsin = std::nullopt,
            .mInterestType = InterestType::BrokerInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-02-28"),
                          .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                     .mGrossAmount = 9'000'000, // EUR 900.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR,
                     .mCashHolder = CashHolder::Bank},
                },
        },
        {
            .mName = "Interactive Brokers",
            .mIsin = std::nullopt,
            .mInterestType = InterestType::BrokerInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-07-01"),
                          .mSources = {source(Broker::InteractiveBrokers, 2, "synthetic-ib-1")}},
                     .mGrossAmount = 2'000'000, // EUR 200.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR,
                     .mCashHolder = CashHolder::Broker},
                },
        },
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // EUR 900.00 + EUR 200.00 would be above the allowance, but the IBKR interest is other
    // interest, so the total stays EUR 900.00 and the question is asked. Without an answer the
    // row holds the EUR 900.00 only.
    EXPECT_EQ(run.mLast.mQualifyingTotal, std::optional<Money>{9'000'000});
    ASSERT_EQ(run.mLast.mDepositReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(0).mInterest, 9'000'000);
    EXPECT_EQ(run.result().mRequests.size(), 1U);
}

// --- Amounts ---

TEST(InterestPaymentsTest, DepositTaxIsSummedAndABlankTaxCountsZeroWithoutADiagnostic) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-28"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 6'000'000, // EUR 600.00
                 .mTaxPaid = 2'500,         // EUR 0.25
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
                {.mMetadata = {.mTaxDate = day("2025-03-31"),
                               .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                 .mGrossAmount = 6'000'000, // EUR 600.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDepositReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(0).mInterest, 12'000'000);
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(0).mForeignTax, 2'500);
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
}

TEST(InterestPaymentsTest, ACouponInAForeignCurrencyConvertsGrossAndTaxAtTheReceiptDateRate) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer A,TAX-A,1 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    addFile2025(rates);
    addRate(rates, "USD", "2025-03-14", 108'800'000, 3); // 1 EUR = 1.0880 USD
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 100'000, // USD 10.00
                 .mTaxPaid = 15'000,      // USD 1.50
                 .mCurrency = Currency::USD,
                 .mTaxCurrency = Currency::USD},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);

    const auto& row = run.mLast.mOtherReport.mRows.at(0);

    // USD 10.00 / 1.0880 = EUR 9.19117647, rounded once to 9.1912; USD 1.50 / 1.0880 = 1.3787.
    EXPECT_EQ(row.mGrossInterest, 91'912);
    EXPECT_EQ(row.mWithholding.mForeignTax, 13'787);
    EXPECT_FALSE(row.mWithholding.mBlankInExport);
    ASSERT_EQ(run.mLast.mUsedRates.size(), 1U);
    EXPECT_EQ(run.mLast.mUsedRates.at(0).mRate.mValue, 108'800'000);
}

TEST(InterestPaymentsTest, ADepositPaymentWithoutARateGivesNoTotalNoQuestionAndABlockingError) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates; // no file of 2025
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-28"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 100'000, // USD 10.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::USD,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    EXPECT_EQ(run.mLast.mQualifyingTotal, std::nullopt);
    EXPECT_TRUE(run.mLast.mDepositReport.mRows.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(statusOf(run.result(), ReportType::BankDepositInterest), ReportStatus::Failed);
    EXPECT_EQ(statusOf(run.result(), ReportType::OtherInterest), ReportStatus::NoData);
}

TEST(InterestPaymentsTest, ADepositTotalThatConvertsToZeroAsksNothing) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    addRate(rates, "JPY", "2025-03-14", 16'000'000'000, 2); // 1 EUR = 160.00000000 JPY
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 1, // JPY 0.0001 = EUR 0.000000625, rounds to 0.0000
                 .mTaxPaid = 0,
                 .mCurrency = Currency::JPY,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // A total of 0 is never asked about and gives no rows and no reason.
    EXPECT_EQ(run.mLast.mQualifyingTotal, std::optional<Money>{0});
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.mLast.mDepositReport.mRows.empty());
    EXPECT_EQ(run.mLast.mDepositNoDataReason, std::nullopt);
}

TEST(InterestPaymentsTest, APaymentOfZeroOrLessIsNeverARowOrPartOfTheTotal) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments =
        {
            {
                .mName = "",
                .mIsin = std::nullopt,
                .mInterestType = InterestType::BrokerInterest,
                .mTransactions =
                    {
                        {.mMetadata =
                             {.mTaxDate = day("2025-02-28"),
                              .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                         .mGrossAmount = 12'000'000, // EUR 1,200.00
                         .mTaxPaid = 0,
                         .mCurrency = Currency::EUR,
                         .mTaxCurrency = Currency::EUR,
                         .mCashHolder = CashHolder::Bank},
                        {.mMetadata =
                             {.mTaxDate = day("2025-03-31"),
                              .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                         .mGrossAmount = -10'000, // EUR -1.00, debit interest
                         .mTaxPaid = 0,
                         .mCurrency = Currency::EUR,
                         .mTaxCurrency = Currency::EUR,
                         .mCashHolder = CashHolder::Bank},
                        {.mMetadata =
                             {.mTaxDate = day("2022-03-31"),
                              .mSources = {source(Broker::TradeRepublic, 4, "synthetic-int-3")}},
                         .mGrossAmount = 0, // EUR 0.00, outside the year
                         .mTaxPaid = 0,
                         .mCurrency = Currency::EUR,
                         .mTaxCurrency = Currency::EUR,
                         .mCashHolder = CashHolder::Bank},
                        {.mMetadata =
                             {.mTaxDate = day("2025-04-30"),
                              .mSources = {source(Broker::TradeRepublic, 5, "synthetic-int-4")}},
                         .mGrossAmount = 0, // EUR 0.00
                         .mTaxPaid = 0,
                         .mCurrency = Currency::EUR,
                         .mTaxCurrency = Currency::EUR,
                         .mCashHolder = CashHolder::Bank},
                    },
            }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // Two payments of the year are not above 0; the 2022 one is outside the year and silent.
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::InterestPaymentNotPositive),
              2U);
    EXPECT_EQ(run.mLast.mQualifyingTotal, std::optional<Money>{12'000'000});
    ASSERT_EQ(run.mLast.mDepositReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(0).mPayments.size(), 1U);
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(0).mInterest, 12'000'000);

    // Interest on cash has no instrument name, so the warning names the broker.
    const auto warning = std::find_if(
        run.mLast.mDiagnostics.begin(),
        run.mLast.mDiagnostics.end(),
        [](const ProcessingDiagnostic& aDiagnostic) {
            return aDiagnostic.mCode == ProcessingDiagnosticCode::InterestPaymentNotPositive;
        });

    ASSERT_NE(warning, run.mLast.mDiagnostics.end());
    EXPECT_EQ(warning->mContext.mInstrumentName, std::optional<std::string>{"Trade Republic"});
}

TEST(InterestPaymentsTest, ACouponThatRoundsToZeroCentsIsNotARow) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer A,TAX-A,1 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-06-15"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 40, // EUR 0.0040 rounds to 0.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-16"),
                               .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                 .mGrossAmount = 50, // EUR 0.0050 rounds to 0.01
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mDate, day("2025-06-16"));
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::InterestPaymentNotPositive),
              1U);
}

// --- Payers of coupons ---

TEST(InterestPaymentsTest, ACouponIssuerThatIsNotFoundIsReportedWithoutFactsAndOneWarningPerIsin) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-09-14"),
                               .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mPayer, IncomePayer{});
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(1).mPayer, IncomePayer{});
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mSourceCountry, std::nullopt);
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::InterestPayerNotFound), 1U);
    EXPECT_TRUE(run.result().mRequests.empty());
}

TEST(InterestPaymentsTest, ASlovenianIssuerCouponIsARowWithTheIssuersFacts) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer SI,TAX-S,1 Main Street,SI,,,issuer,manual,\n"
                    "XX0000000028,Issuer A,TAX-A,2 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {
        {
            .mName = "Synthetic Bond SI",
            .mIsin = "XX0000000010",
            .mInterestType = InterestType::BondInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-03-14"),
                          .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                     .mGrossAmount = 250'000, // EUR 25.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Bond A",
            .mIsin = "XX0000000028",
            .mInterestType = InterestType::BondInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-03-15"),
                          .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                     .mGrossAmount = 300'000, // EUR 30.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR},
                },
        },
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // Both coupons are rows, in ledger order. The payer does not withhold on a listed Slovenian
    // bond, so the user reports it: nothing is left out.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);

    const auto& slovenian = run.mLast.mOtherReport.mRows.at(0);

    EXPECT_EQ(std::get<BondCoupon>(slovenian.mKind).mIsin, "XX0000000010");
    EXPECT_EQ(slovenian.mGrossInterest, 250'000);
    EXPECT_EQ(slovenian.mPayer.mName, "Issuer SI");
    EXPECT_EQ(slovenian.mPayer.mCountry, "SI");
    EXPECT_EQ(std::get<BondCoupon>(run.mLast.mOtherReport.mRows.at(1).mKind).mIsin, "XX0000000028");
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(1).mGrossInterest, 300'000);

    // A tax of 0 needs no warning.
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::SlovenianCouponTax), 0U);
    EXPECT_TRUE(run.result().mExcludedItems.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
}

TEST(InterestPaymentsTest, ATaxOnASlovenianIssuerCouponIsARowWithAWarning) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer SI,TAX-S,1 Main Street,SI,,,issuer,manual,\n"
                    "XX0000000028,Issuer A,TAX-A,2 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {
        {
            .mName = "Synthetic Bond SI",
            .mIsin = "XX0000000010",
            .mInterestType = InterestType::BondInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-03-14"),
                          .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                     .mGrossAmount = 250'000, // EUR 25.00
                     .mTaxPaid = 62'500,      // EUR 6.25
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Bond A",
            .mIsin = "XX0000000028",
            .mInterestType = InterestType::BondInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-03-15"),
                          .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                     .mGrossAmount = 300'000, // EUR 30.00
                     .mTaxPaid = 75'000,      // EUR 7.50
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR},
                },
        },
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // Both coupons are rows. Only the Slovenian issuer's coupon with tax gets the warning; the
    // French issuer's tax is a normal foreign tax.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mWithholding.mForeignTax, 62'500);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(1).mWithholding.mForeignTax, 75'000);
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::SlovenianCouponTax), 1U);

    const auto warning =
        std::find_if(run.mLast.mDiagnostics.begin(),
                     run.mLast.mDiagnostics.end(),
                     [](const ProcessingDiagnostic& aDiagnostic) {
                         return aDiagnostic.mCode == ProcessingDiagnosticCode::SlovenianCouponTax;
                     });

    ASSERT_NE(warning, run.mLast.mDiagnostics.end());
    EXPECT_EQ(warning->mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning->mContext.mIsin, "XX0000000010");
    EXPECT_EQ(warning->mContext.mAffectedReports,
              std::vector<ReportType>{ReportType::OtherInterest});
}

TEST(InterestPaymentsTest, DisagreeingIssuerRowsAskAPayerChoiceThatDoesNotStopDohObr) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer Y,TAX-Y,2 Main Street,FR,,,wikidata,manual,\n"
                    "XX0000000010,Issuer X,TAX-X,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // No answer: the row has no payer facts, with the warning, and the form is not `needs_input`.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mPayer, IncomePayer{});
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::InterestPayerNotFound), 1U);
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mType, RequestType::IncomeDetailsRequired);
    EXPECT_EQ(run.result().mRequests.at(0).mAffectedReports,
              std::vector<ReportType>{ReportType::OtherInterest});
    EXPECT_NE(statusOf(run.result(), ReportType::OtherInterest), ReportStatus::NeedsInput);

    // Option 0 is "Issuer X" (sorted by name); picking it gives its facts and no warning.
    const auto rejections = run.answerFirst(PayerChoiceAnswer{.mPickedOption = 0});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mPayer.mName,
              std::optional<std::string>{"Issuer X"});
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mSourceCountry, std::optional<std::string>{"DE"});
}

TEST(InterestPaymentsTest, SkippingTheIssuerChoiceGivesTheSameRowAsNotFound) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer Y,TAX-Y,2 Main Street,FR,,,wikidata,manual,\n"
                    "XX0000000010,Issuer X,TAX-X,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(PayerChoiceAnswer{.mPickedOption = std::nullopt});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mPayer, IncomePayer{});
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::InterestPayerNotFound), 1U);
}

TEST(InterestPaymentsTest, DepositRowsOfTwoBrokersAreSortedByPayerName) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {
        {
            .mName = "Interactive Brokers",
            .mIsin = std::nullopt,
            .mInterestType = InterestType::BrokerInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-02-28"),
                          .mSources = {source(Broker::InteractiveBrokers, 2, "synthetic-ib-1")}},
                     .mGrossAmount = 6'000'000, // EUR 600.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR,
                     .mCashHolder = CashHolder::Bank},
                },
        },
        {
            .mName = "",
            .mIsin = std::nullopt,
            .mInterestType = InterestType::BrokerInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-03-31"),
                          .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                     .mGrossAmount = 6'000'000, // EUR 600.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR,
                     .mCashHolder = CashHolder::Bank},
                },
        },
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // EUR 600.00 + EUR 600.00 = EUR 1,200.00 is above the allowance. Interactive Brokers has two
    // companies in the file, so its row has no payer; empty sorts before "Broker T".
    EXPECT_EQ(run.mLast.mQualifyingTotal, std::optional<Money>{12'000'000});
    ASSERT_EQ(run.mLast.mDepositReport.mRows.size(), 2U);
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(0).mPayer, IncomePayer{});
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(0).mInterest, 6'000'000);
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(1).mPayer.mName,
              std::optional<std::string>{"Broker T"});
    EXPECT_EQ(run.mLast.mDepositReport.mRows.at(1).mInterest, 6'000'000);
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::InterestPayerNotFound), 1U);
}

TEST(InterestPaymentsTest, ADepositTotalThatOverflowsGivesNoTotalNoQuestionAndAnError) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-02-28"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 5'000'000'000'000'000'000, // 5 * 10^18
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
                {.mMetadata = {.mTaxDate = day("2025-03-31"),
                               .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
                 .mGrossAmount = 5'000'000'000'000'000'000, // 5 * 10^18; the sum is above int64
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR,
                 .mCashHolder = CashHolder::Bank},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    EXPECT_EQ(run.mLast.mQualifyingTotal, std::nullopt);
    EXPECT_TRUE(run.mLast.mDepositReport.mRows.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    ASSERT_FALSE(run.mLast.mDiagnostics.empty());
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mAffectedReports,
              std::vector<ReportType>{ReportType::BankDepositInterest});
    EXPECT_EQ(statusOf(run.result(), ReportType::BankDepositInterest), ReportStatus::Failed);
}

TEST(InterestPaymentsTest, AnInterestTypeNothingHandlesYetIsLeftOut) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Unknown Interest",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
    EXPECT_TRUE(run.mLast.mDepositReport.mRows.empty());
    EXPECT_EQ(run.mLast.mQualifyingTotal, std::nullopt);
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
}

// --- Order and empty input ---

TEST(InterestPaymentsTest, DohObrRowsFollowTheLedgerOrderOfTwoInstruments) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer A,TAX-A,1 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {
        {
            .mName = "Synthetic Bond A",
            .mIsin = "XX0000000010",
            .mInterestType = InterestType::BondInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-09-01"),
                          .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                     .mGrossAmount = 100'000, // EUR 10.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Interactive Brokers",
            .mIsin = std::nullopt,
            .mInterestType = InterestType::BrokerInterest,
            .mTransactions =
                {
                    {.mMetadata =
                         {.mTaxDate = day("2025-09-01"),
                          .mSources = {source(Broker::InteractiveBrokers, 3, "synthetic-ib-1")}},
                     .mGrossAmount = 200'000, // EUR 20.00, same day, the second instrument
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR,
                     .mCashHolder = CashHolder::Broker},
                    {.mMetadata =
                         {.mTaxDate = day("2025-02-01"),
                          .mSources = {source(Broker::InteractiveBrokers, 4, "synthetic-ib-2")}},
                     .mGrossAmount = 300'000, // EUR 30.00, earlier date listed later
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR,
                     .mCashHolder = CashHolder::Broker},
                },
        },
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 3U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 300'000); // 1 February
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(1).mGrossInterest, 100'000); // 1 September, bond
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(2).mGrossInterest, 200'000); // 1 September, cash
}

TEST(InterestPaymentsTest, AStatementWithoutInterestGivesAnEmptyResult) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    RunDecisions decisions;

    const auto result =
        prepareInterestPayments(statement, ledgerOf(2025), catalogue, rates, decisions);

    EXPECT_TRUE(result.mDepositReport.mRows.empty());
    EXPECT_TRUE(result.mOtherReport.mRows.empty());
    EXPECT_EQ(result.mQualifyingTotal, std::nullopt);
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mUsedRates.empty());
}

// --- The Doh-Obr row check: a value the form cannot hold is an error of its unit ---

/// A catalogue with the given payer rows, a Trade Republic payer with the given tax number and one
/// Interactive Brokers payer.
ReferenceCatalogue catalogueWithTradeRepublicNumber(std::string_view aPayerRows,
                                                    std::string_view aTradeRepublicNumber) {
    const std::string payers = std::string{kPayersHeader} + std::string{aPayerRows};
    const std::string relief = std::string{kReliefHeader} + "BE,dividend,Treaty text,manual,\n";
    const std::string broker = std::string{kBrokerHeader} + "TR,TR-X,Broker T," +
                               std::string{aTradeRepublicNumber} +
                               ",\"1 Main Street, Berlin\",DE,manual,\n"
                               "IBKR,IB-X,Broker X,123,1 Side Street,GB,manual,\n";

    return parseReferenceCatalogue(payers, relief, broker);
}

TEST(InterestPaymentsTest, ACouponWhoseIssuerNumberIsTooLongForTheFormIsAnErrorOfItsIsin) {
    // The identification number of Doh-Obr has at most 30 characters; this one has 31. The error is
    // found here, so the user can leave the ISIN out before the XML is written.
    const std::string longNumber(31, 'A');
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer A," + longNumber +
                    ",1 Main Street,FR,,,issuer,manual,\n"
                    "XX0000000028,Issuer B,TAX-B,2 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 100'000, // EUR 10.00
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
        {.mName = "Synthetic Bond B",
         .mIsin = "XX0000000028",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                             .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 50'000, // EUR 5.00
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);

    const auto& error = run.mLast.mDiagnostics.at(0);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(error.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(error.mContext.mField, std::optional<std::string>{"IdentificationNumber"});
    EXPECT_EQ(error.mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_EQ(error.mContext.mCashInterestBroker, std::nullopt);
    EXPECT_EQ(error.mContext.mAffectedReports, std::vector<ReportType>{ReportType::OtherInterest});

    // No row for Bond A; the row of Bond B (EUR 5.00) stays.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(std::get<BondCoupon>(run.mLast.mOtherReport.mRows.at(0).mKind).mIsin, "XX0000000028");
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 50'000);
}

TEST(InterestPaymentsTest, ACashInterestWhoseBrokerNumberIsTooLongIsAnErrorOfThatBroker) {
    // The broker payer row of Trade Republic has a number of 31 characters; Interactive Brokers is
    // fine. The error names Trade Republic and has no ISIN.
    const std::string longNumber(31, 'A');
    const auto catalogue = catalogueWithTradeRepublicNumber("", longNumber);
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 30'000, // EUR 3.00
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR,
               .mCashHolder = CashHolder::Broker}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions = {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                                          .mSources = {source(Broker::InteractiveBrokers,
                                                              3,
                                                              "synthetic-int-2")}},
                            .mGrossAmount = 40'000, // EUR 4.00
                            .mTaxPaid = 0,
                            .mCurrency = Currency::EUR,
                            .mTaxCurrency = Currency::EUR,
                            .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);

    const auto& error = run.mLast.mDiagnostics.at(0);

    EXPECT_EQ(error.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(error.mContext.mField, std::optional<std::string>{"IdentificationNumber"});
    EXPECT_EQ(error.mContext.mIsin, std::nullopt);
    EXPECT_EQ(error.mContext.mCashInterestBroker, std::optional<Broker>{Broker::TradeRepublic});
    EXPECT_EQ(error.mContext.mAffectedReports, std::vector<ReportType>{ReportType::OtherInterest});

    // Only the row of Interactive Brokers (EUR 4.00) is made.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 40'000);
}

TEST(InterestPaymentsTest, ACashInterestAboveTheLargestValueOfTheFormIsAnErrorOfItsBroker) {
    // EUR 999999999999.9950 rounds to 1000000000000.00, which has 13 digits: the form holds 12.
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 9'999'999'999'999'950,
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR,
               .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mField, std::optional<std::string>{"Value"});
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mCashInterestBroker,
              std::optional<Broker>{Broker::TradeRepublic});
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
}

TEST(InterestPaymentsTest, TheLargestValueThatFitsTheFormIsARow) {
    // EUR 999999999999.9949 rounds to 999999999999.99: the largest value, still a row.
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 9'999'999'999'999'949,
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR,
               .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mGrossInterest, 9'999'999'999'999'949);
}

TEST(InterestPaymentsTest, AValidRowOfEachSubtypeGivesNoRowError) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer A,TAX-A,1 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 100'000, // EUR 10.00
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                             .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 30'000, // EUR 3.00
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR,
               .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
    EXPECT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);
    EXPECT_EQ(std::get<BrokerCashInterest>(run.mLast.mOtherReport.mRows.at(1).mKind).mBroker,
              Broker::TradeRepublic);
}

TEST(InterestPaymentsTest, ACouponWhoseIssuerNameIsNotValidXmlTextIsAnErrorOfItsIsin) {
    // A control character in the issuer's name cannot be written to the XML.
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer\x01 A,TAX-A,1 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 100'000, // EUR 10.00
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mField, std::optional<std::string>{"Name"});
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
}

TEST(InterestPaymentsTest, ACouponWhoseIssuerAddressIsNotValidXmlTextIsAnErrorOfItsIsin) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer A,TAX-A,1 Main\x02 Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 100'000, // EUR 10.00
               .mTaxPaid = 0,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mField, std::optional<std::string>{"Address"});
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
}

TEST(InterestPaymentsTest, ACouponWithAForeignTaxAboveTheLargestValueOfTheFormIsAnErrorOfItsIsin) {
    // Tax EUR 999999999999.9950 rounds to 1000000000000.00, which has 13 digits.
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer A,TAX-A,1 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 100'000, // EUR 10.00
               .mTaxPaid = 9'999'999'999'999'950,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mField,
              std::optional<std::string>{"ForeignTax"});
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.empty());
}

TEST(InterestPaymentsTest, ASlovenianIssuersForeignTaxIsNotCheckedBecauseItIsNotWritten) {
    // The issuer's country is SI, so the huge tax is not written and is no error of the row (the
    // warning about a Slovenian coupon with tax is given instead).
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer A,SI12345674,1 Main Street,SI,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;

    addFile2025(rates);
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond A",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 100'000, // EUR 10.00
               .mTaxPaid = 9'999'999'999'999'950,
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mPayer.mTaxNumber,
              std::optional<std::string>{"12345674"});
    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::SlovenianCouponTax);
}

// --- The treaty claim of Doh-Obr interest ---

/// The treaty claim question data a run is expected to ask.
InterestTreatyClaimQuestion
treatyQuestion(std::string aCountry, std::vector<Isin> aIsins, bool aCashInterest) {
    return InterestTreatyClaimQuestion{.mYear = 2025,
                                       .mCountry = std::move(aCountry),
                                       .mIsins = std::move(aIsins),
                                       .mCashInterest = aCashInterest};
}

TEST(InterestPaymentsTest, ATaxedForeignCouponAsksTheTreatyClaimQuestionWithoutBlocking) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer US,TAX-U,1 Main Street,US,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond US",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 30'000,      // EUR 3.00
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // No answer: one request for the country, no claim and no warning, and Doh-Obr is not blocked.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_FALSE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::TreatyClaimDetailsMissing),
              0U);

    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    EXPECT_EQ(request.mType, RequestType::IncomeDetailsRequired);
    EXPECT_EQ(request.mAffectedReports, std::vector<ReportType>{ReportType::OtherInterest});
    EXPECT_EQ(request.mData, QuestionData{treatyQuestion("US", {"XX0000000010"}, false)});
    EXPECT_NE(statusOf(run.result(), ReportType::OtherInterest), ReportStatus::NeedsInput);
}

TEST(InterestPaymentsTest, YesClaimsTheCountryRowsWithoutATextAndWarnsOnce) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer US,TAX-U,1 Main Street,US,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond US",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 30'000,      // EUR 3.00
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(InterestTreatyClaimAnswer{.mClaim = true});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.result().mRequests.empty());

    // The claim has no text and no reference row: the paragraph and article are filled in eDavki.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    ASSERT_TRUE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim->mStatement, std::nullopt);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim->mCatalogueRow, std::nullopt);

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);

    const auto& warning = run.mLast.mDiagnostics.at(0);

    EXPECT_EQ(warning.mCode, ProcessingDiagnosticCode::TreatyClaimDetailsMissing);
    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mMessageKey,
              std::optional<MessageKey>{MessageKey::TreatyClaimDetailsMissing});
    EXPECT_EQ(warning.mMessageParams, std::vector{textParam("country", "US")});
    EXPECT_EQ(warning.mContext.mAffectedReports,
              std::vector<ReportType>{ReportType::OtherInterest});
    EXPECT_EQ(warning.mContext.mTaxDate, std::optional<Date>{day("2025-03-14")});
}

TEST(InterestPaymentsTest, NoLeavesTheRowsWithoutAClaimAndWithoutAWarning) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer US,TAX-U,1 Main Street,US,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond US",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 30'000,      // EUR 3.00
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(InterestTreatyClaimAnswer{.mClaim = false});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_FALSE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
}

TEST(InterestPaymentsTest, ACouponWithABlankTaxAsksNoTreatyQuestion) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer US,TAX-U,1 Main Street,US,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond US",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000,  // EUR 25.00
                 .mTaxPaid = std::nullopt, // blank: no foreign tax
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
}

TEST(InterestPaymentsTest, ACouponWithoutAPayerCountryAsksNoTreatyQuestion) {
    // No reference row for the ISIN: the payer facts are empty, so there is no country to claim
    // for.
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond A",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 30'000,      // EUR 3.00
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::InterestPayerNotFound), 1U);
}

TEST(InterestPaymentsTest, ATaxOnASlovenianIssuerCouponAsksNoTreatyQuestion) {
    // The tax of a Slovenian issuer is not foreign tax, so no treaty applies.
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer SI,TAX-S,1 Main Street,SI,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond SI",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 62'500,      // EUR 6.25
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::SlovenianCouponTax), 1U);
}

TEST(InterestPaymentsTest, CashInterestAndACouponOfTheSameCountryShareOneTreatyQuestion) {
    // The broker payer of Trade Republic is in DE, and so is the issuer of the coupon.
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer DE,TAX-D,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond DE",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 250'000, // EUR 25.00
               .mTaxPaid = 30'000,      // EUR 3.00
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                             .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 100'000, // EUR 10.00
               .mTaxPaid = 10'000,      // EUR 1.00
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR,
               .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mData,
              QuestionData{treatyQuestion("DE", {"XX0000000010"}, true)});

    const auto rejections = run.answerFirst(InterestTreatyClaimAnswer{.mClaim = true});

    EXPECT_TRUE(rejections.empty());

    // One answer claims both rows, with one warning for the country.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.at(1).mTreatyClaim.has_value());
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::TreatyClaimDetailsMissing),
              1U);
}

TEST(InterestPaymentsTest, CashInterestWithTaxAloneAsksAQuestionWithoutIsins) {
    const auto catalogue = catalogueOf("");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "",
        .mIsin = std::nullopt,
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions =
            {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                            .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
              .mGrossAmount = 100'000, // EUR 10.00
              .mTaxPaid = 10'000,      // EUR 1.00
              .mCurrency = Currency::EUR,
              .mTaxCurrency = Currency::EUR,
              .mCashHolder = CashHolder::Broker}},
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mData, QuestionData{treatyQuestion("DE", {}, true)});
}

TEST(InterestPaymentsTest, TheTreatyQuestionsOfTwoCountriesAreAskedAndAnsweredSeparately) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer US,TAX-U,1 Main Street,US,,,issuer,manual,\n"
                    "XX0000000028,Issuer FR,TAX-F,2 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond US",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 250'000, // EUR 25.00
               .mTaxPaid = 30'000,      // EUR 3.00
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
        {.mName = "Synthetic Bond FR",
         .mIsin = "XX0000000028",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                             .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 200'000, // EUR 20.00
               .mTaxPaid = 20'000,      // EUR 2.00
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // The result lists the requests by date: the US coupon (2025-03-14), then the FR coupon
    // (2025-03-15).
    ASSERT_EQ(run.result().mRequests.size(), 2U);
    EXPECT_EQ(run.result().mRequests.at(0).mData,
              QuestionData{treatyQuestion("US", {"XX0000000010"}, false)});
    EXPECT_EQ(run.result().mRequests.at(1).mData,
              QuestionData{treatyQuestion("FR", {"XX0000000028"}, false)});

    // Yes for US only: the US coupon (first row) is claimed, the FR one still asks.
    EXPECT_TRUE(run.answerFirst(InterestTreatyClaimAnswer{.mClaim = true}).empty());
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mData,
              QuestionData{treatyQuestion("FR", {"XX0000000028"}, false)});
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);
    EXPECT_TRUE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());
    EXPECT_FALSE(run.mLast.mOtherReport.mRows.at(1).mTreatyClaim.has_value());
}

TEST(InterestPaymentsTest, ADividendTreatyAnswerDoesNotAnswerTheInterestTreatyQuestion) {
    const auto catalogue =
        catalogueOf("XX0000000010,Issuer US,TAX-U,1 Main Street,US,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond US",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 30'000,      // EUR 3.00
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // The session takes only a yes or no for this question; a dividend-style text is rejected.
    const auto rejections =
        run.answerFirst(TreatyClaimAnswer{.mStatement = std::string{"2. odstavek 11. člena"}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::AnswerDoesNotFit);
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_FALSE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());
}

// --- Treaty texts of the reference files: one kind of income each ---

/// A catalogue with the given payer rows and relief rows, one broker payer for Trade Republic (in
/// DE) and two for Interactive Brokers.
ReferenceCatalogue catalogueWithReliefOf(std::string_view aPayerRows,
                                         std::string_view aReliefRows) {
    const std::string payers = std::string{kPayersHeader} + std::string{aPayerRows};
    const std::string relief = std::string{kReliefHeader} + std::string{aReliefRows};
    const std::string broker = std::string{kBrokerHeader} +
                               "TR,TR-X,Broker T,TAX-T,\"1 Main Street, Berlin\",DE,manual,\n"
                               "IBKR,IB-X,Broker X,123,1 Side Street,GB,manual,\n"
                               "IBKR,IB-Y,Broker Y,456,2 Side Street,IE,manual,\n";

    return parseReferenceCatalogue(payers, relief, broker);
}

TEST(InterestPaymentsTest, ABondCouponTextOfThePayerCountryIsTheClaimOfATaxedCoupon) {
    // The row 2 of the relief file is the text of coupons from US.
    const auto catalogue =
        catalogueWithReliefOf("XX0000000010,Issuer US,TAX-U,1 Main Street,US,,,issuer,manual,\n",
                              "US,bond_coupon,2. odstavek 11. člena,manual,\n");
    OfficialRateTable rates;

    // One bond, ISIN XX0000000010, with a coupon of EUR 25.00 and EUR 3.00 tax on 2025-03-14.
    MergedStatement statement;

    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 30'000,      // EUR 3.00
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // The claim has the reference text and its row; there is no question and no warning.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    ASSERT_TRUE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());

    const auto& claim = *run.mLast.mOtherReport.mRows.at(0).mTreatyClaim;

    EXPECT_EQ(claim.mStatement, std::optional<std::string>{"2. odstavek 11. člena"});
    ASSERT_TRUE(claim.mCatalogueRow.has_value());
    EXPECT_EQ(claim.mCatalogueRow->mFile, "relief_statements.csv");
    EXPECT_EQ(claim.mCatalogueRow->mRowNumber, 2U);

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
}

TEST(InterestPaymentsTest, ADividendAndACashInterestTextAreNotUsedForACoupon) {
    // US has a text for dividends and one for cash interest, but none for coupons.
    const auto catalogue =
        catalogueWithReliefOf("XX0000000010,Issuer US,TAX-U,1 Main Street,US,,,issuer,manual,\n",
                              "US,dividend,Dividend text,manual,\n"
                              "US,cash_interest,Cash interest text,manual,\n");
    OfficialRateTable rates;

    // One bond, ISIN XX0000000010, with a coupon of EUR 25.00 and EUR 3.00 tax on 2025-03-14.
    MergedStatement statement;

    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 30'000,      // EUR 3.00
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_FALSE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mData,
              QuestionData{treatyQuestion("US", {"XX0000000010"}, false)});
}

TEST(InterestPaymentsTest, TwoDifferentBondCouponTextsOfACountryAskTheQuestion) {
    const auto catalogue =
        catalogueWithReliefOf("XX0000000010,Issuer US,TAX-U,1 Main Street,US,,,issuer,manual,\n",
                              "US,bond_coupon,2. odstavek 11. člena,manual,\n"
                              "US,bond_coupon,3. odstavek 11. člena,manual,\n");
    OfficialRateTable rates;

    // One bond, ISIN XX0000000010, with a coupon of EUR 25.00 and EUR 3.00 tax on 2025-03-14.
    MergedStatement statement;

    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 30'000,      // EUR 3.00
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_FALSE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mData,
              QuestionData{treatyQuestion("US", {"XX0000000010"}, false)});
}

TEST(InterestPaymentsTest, ACouponTextClaimsTheCouponAndTheQuestionCoversOnlyTheCashInterest) {
    // DE has a coupon text but no cash interest text. The issuer and Trade Republic are in DE.
    const auto catalogue =
        catalogueWithReliefOf("XX0000000010,Issuer DE,TAX-D,1 Main Street,DE,,,issuer,manual,\n",
                              "DE,bond_coupon,2. odstavek 11. člena,manual,\n");
    OfficialRateTable rates;

    // The same coupon, and cash interest of Trade Republic of EUR 10.00 with EUR 1.00 tax on
    // 2025-03-15.
    MergedStatement statement;

    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 250'000, // EUR 25.00
               .mTaxPaid = 30'000,      // EUR 3.00
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                             .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 100'000, // EUR 10.00
               .mTaxPaid = 10'000,      // EUR 1.00
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR,
               .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    // The coupon (row 0) is claimed with its text; the cash interest (row 1) is asked.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);
    ASSERT_TRUE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim->mStatement,
              std::optional<std::string>{"2. odstavek 11. člena"});
    EXPECT_FALSE(run.mLast.mOtherReport.mRows.at(1).mTreatyClaim.has_value());

    // The question has no ISIN, because the coupon is not in it, and the date of the cash interest.
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mData, QuestionData{treatyQuestion("DE", {}, true)});
    EXPECT_EQ(run.result().mRequests.at(0).mTaxDate, std::optional<Date>{day("2025-03-15")});

    const auto rejections = run.answerFirst(InterestTreatyClaimAnswer{.mClaim = true});

    EXPECT_TRUE(rejections.empty());

    // Yes claims the cash interest without a text; the coupon keeps its text. One warning.
    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim->mStatement,
              std::optional<std::string>{"2. odstavek 11. člena"});
    ASSERT_TRUE(run.mLast.mOtherReport.mRows.at(1).mTreatyClaim.has_value());
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(1).mTreatyClaim->mStatement, std::nullopt);
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(1).mTreatyClaim->mCatalogueRow, std::nullopt);
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::TreatyClaimDetailsMissing),
              1U);
}

TEST(InterestPaymentsTest, ACashInterestTextClaimsTheCashInterestAndTheQuestionCoversTheCoupon) {
    // DE has a cash interest text but no coupon text.
    const auto catalogue =
        catalogueWithReliefOf("XX0000000010,Issuer DE,TAX-D,1 Main Street,DE,,,issuer,manual,\n",
                              "DE,cash_interest,1. odstavek 11. člena,manual,\n");
    OfficialRateTable rates;

    // The same coupon, and cash interest of Trade Republic of EUR 10.00 with EUR 1.00 tax on
    // 2025-03-15.
    MergedStatement statement;

    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 250'000, // EUR 25.00
               .mTaxPaid = 30'000,      // EUR 3.00
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                             .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 100'000, // EUR 10.00
               .mTaxPaid = 10'000,      // EUR 1.00
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR,
               .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);
    EXPECT_FALSE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());
    ASSERT_TRUE(run.mLast.mOtherReport.mRows.at(1).mTreatyClaim.has_value());
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(1).mTreatyClaim->mStatement,
              std::optional<std::string>{"1. odstavek 11. člena"});

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mData,
              QuestionData{treatyQuestion("DE", {"XX0000000010"}, false)});
    EXPECT_EQ(run.result().mRequests.at(0).mTaxDate, std::optional<Date>{day("2025-03-14")});
}

TEST(InterestPaymentsTest, EachKindOfInterestGetsTheTextOfItsOwnKindWithoutAQuestion) {
    // DE has a different text for coupons and for cash interest.
    const auto catalogue =
        catalogueWithReliefOf("XX0000000010,Issuer DE,TAX-D,1 Main Street,DE,,,issuer,manual,\n",
                              "DE,bond_coupon,Coupon text,manual,\n"
                              "DE,cash_interest,Cash interest text,manual,\n");
    OfficialRateTable rates;

    // The same coupon, and cash interest of Trade Republic of EUR 10.00 with EUR 1.00 tax on
    // 2025-03-15.
    MergedStatement statement;

    statement.mPresentation.mInterestInstruments = {
        {.mName = "Synthetic Bond",
         .mIsin = "XX0000000010",
         .mInterestType = InterestType::BondInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-14"),
                             .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
               .mGrossAmount = 250'000, // EUR 25.00
               .mTaxPaid = 30'000,      // EUR 3.00
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR}}},
        {.mName = "",
         .mIsin = std::nullopt,
         .mInterestType = InterestType::BrokerInterest,
         .mTransactions =
             {{.mMetadata = {.mTaxDate = day("2025-03-15"),
                             .mSources = {source(Broker::TradeRepublic, 3, "synthetic-int-2")}},
               .mGrossAmount = 100'000, // EUR 10.00
               .mTaxPaid = 10'000,      // EUR 1.00
               .mCurrency = Currency::EUR,
               .mTaxCurrency = Currency::EUR,
               .mCashHolder = CashHolder::Broker}}},
    };

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 2U);
    ASSERT_TRUE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());
    ASSERT_TRUE(run.mLast.mOtherReport.mRows.at(1).mTreatyClaim.has_value());
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim->mStatement,
              std::optional<std::string>{"Coupon text"});
    EXPECT_EQ(run.mLast.mOtherReport.mRows.at(1).mTreatyClaim->mStatement,
              std::optional<std::string>{"Cash interest text"});

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
}

TEST(InterestPaymentsTest, ACouponTextOfAnotherCountryIsNotUsed) {
    // The coupon is from US; the only coupon text is for DE.
    const auto catalogue =
        catalogueWithReliefOf("XX0000000010,Issuer US,TAX-U,1 Main Street,US,,,issuer,manual,\n",
                              "DE,bond_coupon,2. odstavek 11. člena,manual,\n");
    OfficialRateTable rates;

    // One bond, ISIN XX0000000010, with a coupon of EUR 25.00 and EUR 3.00 tax on 2025-03-14.
    MergedStatement statement;

    statement.mPresentation.mInterestInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000010",
        .mInterestType = InterestType::BondInterest,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(Broker::TradeRepublic, 2, "synthetic-int-1")}},
                 .mGrossAmount = 250'000, // EUR 25.00
                 .mTaxPaid = 30'000,      // EUR 3.00
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);

    InterestRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mOtherReport.mRows.size(), 1U);
    EXPECT_FALSE(run.mLast.mOtherReport.mRows.at(0).mTreatyClaim.has_value());
    ASSERT_EQ(run.result().mRequests.size(), 1U);
}

} // namespace
