#include "processors/dividend_payments.hpp"

#include "taxbroker/currency.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

// All payers, ISINs, dates, amounts and rates in this test suite are synthetic. The synthetic
// ISINs use the prefix XX and a valid check digit (XX0000000010, XX0000000028).

constexpr std::string_view kPayersHeader =
    "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
    "source_revision\n";
constexpr std::string_view kReliefHeader = "country,statement,source,source_revision\n";
constexpr std::string_view kBrokerHeader =
    "broker,code,name,tax_number,address,country,source,source_revision\n";

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

ReportingYearLedger ledger2025() {
    return ReportingYearLedger{.mYear = 2025,
                               .mYearStart = day("2025-01-01"),
                               .mYearEnd = day("2025-12-31")};
}

/// A catalogue with the given payer rows, one relief statement (BE) and one broker payer.
ReferenceCatalogue catalogueOf(std::string_view aPayerRows) {
    const std::string payers = std::string{kPayersHeader} + std::string{aPayerRows};
    const std::string relief = std::string{kReliefHeader} + "BE,Treaty text,manual,\n";
    const std::string broker =
        std::string{kBrokerHeader} + "IBKR,IB-X,Broker X,123,1 Main Street,GB,manual,\n";

    return parseReferenceCatalogue(payers, relief, broker);
}

/// Marks the rate file of 2025 as loaded, from 2 January to 31 December.
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

/// Puts all dividends in tax-date order. Equal dates keep the order listed in the test.
void orderChronologically(MergedStatement& aStatement) {
    const auto& instruments = aStatement.mPresentation.mDividendInstruments;
    auto& order = aStatement.mChronologicalOrder;

    for (std::size_t instrument = 0; instrument < instruments.size(); ++instrument)
    {
        for (std::size_t payment = 0; payment < instruments[instrument].mTransactions.size();
             ++payment)
        {
            order.push_back({StatementEventKind::Dividend, instrument, payment});
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
struct DividendRun {
    DecisionSession mSession;
    DividendPaymentsResult mLast;

    DividendRun(const MergedStatement& aStatement,
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
                prepareDividendPayments(aStatement, ledger2025(), aCatalogue, aRates, aDecisions);

            ApplicationResultInput input{.mReportingYear = 2025};
            auto copy = mLast;

            appendDividendPayments(input, std::move(copy));

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

/// The status of Doh-Div in a result.
ReportStatus dividendStatus(const ApplicationResult& aResult) {
    return aResult.mReports.at(static_cast<std::size_t>(ReportType::Dividends)).mStatus;
}

/// Counts the diagnostics with this code.
std::size_t countOf(const std::vector<ProcessingDiagnostic>& aDiagnostics,
                    ProcessingDiagnosticCode aCode) {
    return static_cast<std::size_t>(
        std::count_if(aDiagnostics.begin(), aDiagnostics.end(), [aCode](const auto& aDiagnostic) {
            return aDiagnostic.mCode == aCode;
        }));
}

// --- Amounts ---

TEST(DividendPaymentsTest, EurDividendKeepsItsAmountsAndTakesThePayerFacts) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 7'500,      // EUR 0.75
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    // The tax of EUR 0.75 is above 0, so the run has its evidence notice and nothing else.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::ForeignTaxEvidenceRequired);
    ASSERT_EQ(result.mReport.mRows.size(), 1U);

    const auto& row = result.mReport.mRows.at(0);

    EXPECT_EQ(row.mPayment,
              (StatementEventReference{.mKind = StatementEventKind::Dividend,
                                       .mInstrumentIndex = 0,
                                       .mEventIndex = 0}));
    EXPECT_EQ(row.mIsin, "XX0000000010");
    EXPECT_EQ(row.mDate, day("2025-03-14"));
    EXPECT_EQ(row.mKind, DividendKind::Ordinary);
    EXPECT_EQ(row.mGrossIncome, 50'000);
    EXPECT_EQ(row.mWithholding.mForeignTax, 7'500);
    EXPECT_FALSE(row.mWithholding.mBlankInExport);
    EXPECT_EQ(row.mPayer.mName, std::optional<std::string>{"Payer A"});
    EXPECT_EQ(row.mPayer.mAddress, std::optional<std::string>{"1 Main Street"});
    EXPECT_EQ(row.mPayer.mCountry, std::optional<std::string>{"DE"});
    EXPECT_EQ(row.mPayer.mTaxNumber, std::nullopt);
    EXPECT_EQ(row.mPayer.mIdentificationNumber, std::optional<std::string>{"TAX-A"});
    EXPECT_EQ(
        row.mPayer.mCatalogueRow,
        (std::optional<CatalogueSource>{CatalogueSource{.mSource = "manual",
                                                        .mSourceRevision = "",
                                                        .mFile = "payers.csv",
                                                        .mRowNumber = 2,
                                                        .mOrigin = CatalogueOrigin::BuiltIn}}));
    EXPECT_EQ(row.mSourceCountry, std::optional<std::string>{"DE"});
    EXPECT_EQ(row.mTreatyClaim, std::nullopt);
    EXPECT_TRUE(result.mUsedRates.empty());
    EXPECT_TRUE(result.mExcludedItems.empty());
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(DividendPaymentsTest, ForeignGrossWithTaxInEurConvertsOnlyTheGross) {
    // Trade Republic shape: the gross is in the original currency, the tax in EUR.
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    addFile2025(rates);
    addRate(rates, "USD", "2025-03-14", 108'800'000, 3); // 1 EUR = 1.0880 USD
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 100'000, // USD 10.00
                 .mTaxPaid = 13'800,      // EUR 1.38
                 .mCurrency = Currency::USD,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    ASSERT_EQ(result.mReport.mRows.size(), 1U);

    const auto& row = result.mReport.mRows.at(0);

    // USD 10.00 / 1.0880 = EUR 9.19117647, rounded once to 9.1912; the EUR tax is taken as given.
    EXPECT_EQ(row.mGrossIncome, 91'912);
    EXPECT_EQ(row.mWithholding.mForeignTax, 13'800);
    EXPECT_FALSE(row.mWithholding.mBlankInExport);

    ASSERT_EQ(result.mUsedRates.size(), 1U);
    EXPECT_EQ(result.mUsedRates.at(0).mCurrency, Currency::USD);
    EXPECT_EQ(result.mUsedRates.at(0).mTaxDate, day("2025-03-14"));
    EXPECT_EQ(result.mUsedRates.at(0).mRate.mValue, 108'800'000);
    EXPECT_EQ(result.mUsedRates.at(0).mRate.mSourceRow, 3U);
}

TEST(DividendPaymentsTest, ForeignGrossAndForeignTaxEachConvertAtTheRateOfTheReceiptDate) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    addFile2025(rates);
    addRate(rates, "USD", "2025-03-14", 108'800'000, 3); // 1 EUR = 1.0880 USD
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 100'000, // USD 10.00
                 .mTaxPaid = 15'000,      // USD 1.50
                 .mCurrency = Currency::USD,
                 .mTaxCurrency = Currency::USD},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    ASSERT_EQ(result.mReport.mRows.size(), 1U);
    // USD 10.00 / 1.0880 = EUR 9.1912 and USD 1.50 / 1.0880 = EUR 1.37867647, rounded to 1.3787.
    EXPECT_EQ(result.mReport.mRows.at(0).mGrossIncome, 91'912);
    EXPECT_EQ(result.mReport.mRows.at(0).mWithholding.mForeignTax, 13'787);
    // The gross and the tax share one currency and day: one used rate.
    EXPECT_EQ(result.mUsedRates.size(), 1U);
}

TEST(DividendPaymentsTest, ABlankTaxIsAForeignTaxOfZeroWithTheBlankFlagAndNoDiagnostic) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    addFile2025(rates);
    addRate(rates, "USD", "2025-03-14", 108'800'000, 3); // 1 EUR = 1.0880 USD
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 100'000, // USD 10.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::USD,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    ASSERT_EQ(result.mReport.mRows.size(), 1U);
    EXPECT_EQ(result.mReport.mRows.at(0).mGrossIncome, 91'912);
    EXPECT_EQ(result.mReport.mRows.at(0).mWithholding.mForeignTax, 0);
    EXPECT_TRUE(result.mReport.mRows.at(0).mWithholding.mBlankInExport);

    // No diagnostic at all: the payer is known and the tax of 0 needs no evidence notice.
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(DividendPaymentsTest, AnExplicitZeroTaxIsNotBlankAndHasNoWarning) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    ASSERT_EQ(result.mReport.mRows.size(), 1U);
    EXPECT_EQ(result.mReport.mRows.at(0).mWithholding.mForeignTax, 0);
    EXPECT_FALSE(result.mReport.mRows.at(0).mWithholding.mBlankInExport);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(DividendPaymentsTest, OnePaymentOnADayOfTheWeekendUsesTheLastEarlierRateWithAWarning) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    addFile2025(rates);
    addRate(rates, "USD", "2025-03-14", 108'800'000, 3); // Friday, 1 EUR = 1.0880 USD
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                // 2025-03-15 is a Saturday.
                {.mMetadata = {.mTaxDate = day("2025-03-15"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 100'000, // USD 10.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::USD,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    ASSERT_EQ(result.mReport.mRows.size(), 1U);
    EXPECT_EQ(result.mReport.mRows.at(0).mGrossIncome, 91'912); // the rate of Friday
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::OfficialRateFromNearbyDate);
    EXPECT_EQ(result.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
}

TEST(DividendPaymentsTest, AZeroTaxInAForeignCurrencyNeedsNoRate) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    addFile2025(rates);
    addRate(rates, "USD", "2025-03-14", 108'800'000, 3); // 1 EUR = 1.0880 USD; no GBP rate
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 100'000, // USD 10.00
                 .mTaxPaid = 0,           // GBP 0.00, written in the export
                 .mCurrency = Currency::USD,
                 .mTaxCurrency = Currency::GBP},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    // Only the gross needs a rate: USD 10.00 / 1.0880 = EUR 9.1912. The tax of 0 stays 0.
    ASSERT_EQ(result.mReport.mRows.size(), 1U);
    EXPECT_EQ(result.mReport.mRows.at(0).mGrossIncome, 91'912);
    EXPECT_EQ(result.mReport.mRows.at(0).mWithholding.mForeignTax, 0);
    EXPECT_FALSE(result.mReport.mRows.at(0).mWithholding.mBlankInExport);
    EXPECT_EQ(result.mUsedRates.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(DividendPaymentsTest, AForeignPaymentWithoutARateFileHasNoRowAndABlockingError) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates; // no file of 2025
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 100'000, // USD 10.00
                 .mTaxPaid = 13'800,      // EUR 1.38
                 .mCurrency = Currency::USD,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    DividendRun run{statement, catalogue, rates};

    EXPECT_TRUE(run.mLast.mReport.mRows.empty());
    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mMessageKey,
              std::optional<MessageKey>{MessageKey::MissingExchangeRateNoRateFile});
    EXPECT_EQ(dividendStatus(run.result()), ReportStatus::Failed);
}

TEST(DividendPaymentsTest, AnAmountThatDoesNotFitAfterConversionHasNoRowAndAnError) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    addFile2025(rates);
    addRate(rates, "USD", "2025-03-14", 1, 3); // 1 EUR = 0.00000001 USD
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                // Gross 900,000,000,000,000.0000 USD divided by 0.00000001 is 9e22 EUR: no int64.
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 9'000'000'000'000'000'000,
                 .mTaxPaid = 0,
                 .mCurrency = Currency::USD,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    EXPECT_TRUE(result.mReport.mRows.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mAffectedReports,
              std::vector<ReportType>{ReportType::Dividends});
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mField, std::optional<std::string>{"amount"});
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_TRUE(result.mUsedRates.empty());
}

// --- The year and the order ---

TEST(DividendPaymentsTest, OnlyDividendsOfTheReportingYearAreRows) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-12-31"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 10'000, // EUR 1.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-01"),
                               .mSources = {source(3, "synthetic-div-2")}},
                 .mGrossAmount = 20'000, // EUR 2.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-12-31"),
                               .mSources = {source(4, "synthetic-div-3")}},
                 .mGrossAmount = 30'000, // EUR 3.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2026-01-01"),
                               .mSources = {source(5, "synthetic-div-4")}},
                 .mGrossAmount = 40'000, // EUR 4.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    // 1 January and 31 December are in; the day before and the day after are out.
    ASSERT_EQ(result.mReport.mRows.size(), 2U);
    EXPECT_EQ(result.mReport.mRows.at(0).mDate, day("2025-01-01"));
    EXPECT_EQ(result.mReport.mRows.at(0).mGrossIncome, 20'000);
    EXPECT_EQ(result.mReport.mRows.at(1).mDate, day("2025-12-31"));
    EXPECT_EQ(result.mReport.mRows.at(1).mGrossIncome, 30'000);
}

TEST(DividendPaymentsTest, RowsFollowTheLedgerOrderOfTwoInstruments) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n"
                    "XX0000000028,Payer B,TAX-B,2 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {
        {
            .mName = "Synthetic Payer A",
            .mIsin = "XX0000000010",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2025-06-20"),
                                   .mSources = {source(2, "synthetic-a-late")}},
                     .mGrossAmount = 10'000, // EUR 1.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Payer B",
            .mIsin = "XX0000000028",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2025-03-10"),
                                   .mSources = {source(3, "synthetic-b-early")}},
                     .mGrossAmount = 20'000, // EUR 2.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2025-06-20"),
                                   .mSources = {source(4, "synthetic-b-late")}},
                     .mGrossAmount = 30'000, // EUR 3.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR},
                },
        },
    };

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    // By date; the two payments on 20 June keep the order of the instruments in the statement.
    ASSERT_EQ(result.mReport.mRows.size(), 3U);
    EXPECT_EQ(result.mReport.mRows.at(0).mIsin, "XX0000000028");
    EXPECT_EQ(result.mReport.mRows.at(0).mGrossIncome, 20'000);
    EXPECT_EQ(result.mReport.mRows.at(1).mIsin, "XX0000000010");
    EXPECT_EQ(result.mReport.mRows.at(1).mGrossIncome, 10'000);
    EXPECT_EQ(result.mReport.mRows.at(2).mIsin, "XX0000000028");
    EXPECT_EQ(result.mReport.mRows.at(2).mGrossIncome, 30'000);
}

TEST(DividendPaymentsTest, AStatementWithoutDividendsGivesAnEmptyResult) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    EXPECT_TRUE(result.mReport.mRows.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mExcludedItems.empty());
    EXPECT_TRUE(result.mUsedRates.empty());
}

// --- Payers ---

TEST(DividendPaymentsTest, APayerThatIsNotFoundIsReportedWithoutFactsAndOneWarningPerIsin) {
    const auto catalogue =
        catalogueOf("XX0000000028,Payer B,TAX-B,2 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010", // not in the catalogue
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-20"),
                               .mSources = {source(3, "synthetic-div-2")}},
                 .mGrossAmount = 60'000, // EUR 6.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    // Both payments are rows, with the amount and the ISIN but no payer fact.
    ASSERT_EQ(result.mReport.mRows.size(), 2U);

    for (const auto& row : result.mReport.mRows)
    {
        EXPECT_EQ(row.mIsin, "XX0000000010");
        EXPECT_EQ(row.mPayer, IncomePayer{});
        EXPECT_EQ(row.mSourceCountry, std::nullopt);
    }

    // One warning for the ISIN, with the sources of both payments and the first date.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& warning = result.mDiagnostics.at(0);

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, ProcessingDiagnosticCode::DividendPayerNotFound);
    EXPECT_EQ(warning.mMessageKey, std::optional<MessageKey>{MessageKey::DividendPayerNotFound});
    EXPECT_EQ(warning.mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_EQ(warning.mContext.mTaxDate, std::optional<Date>{day("2025-03-14")});
    ASSERT_EQ(warning.mContext.mSources.size(), 2U);
    EXPECT_EQ(warning.mContext.mSources.at(0).mSourceRow, 2U);
    EXPECT_EQ(warning.mContext.mSources.at(1).mSourceRow, 3U);
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(DividendPaymentsTest, RowsOfAnIsinWithTheSameFactsAreOnePayerWithoutAQuestion) {
    // Two rows with the same facts are one found payer (file order: the first row is used).
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n"
                    "XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,wikidata,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    ASSERT_EQ(result.mReport.mRows.size(), 1U);
    EXPECT_EQ(result.mReport.mRows.at(0).mPayer.mName, std::optional<std::string>{"Payer A"});
    EXPECT_EQ(result.mReport.mRows.at(0).mPayer.mCatalogueRow->mRowNumber, 2U);
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(DividendPaymentsTest, DisagreeingRowsAskAPayerChoiceThatDoesNotStopDohDiv) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer Y,TAX-Y,2 Main Street,FR,,,wikidata,manual,\n"
                    "XX0000000010,Payer X,TAX-X,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    DividendRun run{statement, catalogue, rates};

    // The row is made without payer facts, with the same warning as for a payer not found.
    ASSERT_EQ(run.mLast.mReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mReport.mRows.at(0).mPayer, IncomePayer{});
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::DividendPayerNotFound), 1U);

    // One request, `income_details_required`, with both rows sorted by name (X before Y).
    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    EXPECT_EQ(request.mType, RequestType::IncomeDetailsRequired);
    EXPECT_EQ(request.mAffectedReports, std::vector<ReportType>{ReportType::Dividends});
    EXPECT_EQ(request.mIsin, std::optional<Isin>{"XX0000000010"});

    const auto* question = std::get_if<PayerChoiceQuestion>(&request.mData);

    ASSERT_NE(question, nullptr);
    EXPECT_EQ(question->mName, "Synthetic Payer A");
    ASSERT_EQ(question->mOptions.size(), 2U);
    EXPECT_EQ(question->mOptions.at(0).mName, "Payer X");
    EXPECT_EQ(question->mOptions.at(0).mCountry, "DE");
    EXPECT_EQ(question->mOptions.at(0).mTaxNumber, std::optional<std::string>{"TAX-X"});
    EXPECT_EQ(question->mOptions.at(0).mSource.mRowNumber, 3U);
    EXPECT_EQ(question->mOptions.at(1).mName, "Payer Y");
    EXPECT_EQ(question->mOptions.at(1).mSource.mRowNumber, 2U);

    // The open question is listed with Doh-Div, which is not `needs_input` because of it.
    const auto& report = run.result().mReports.at(static_cast<std::size_t>(ReportType::Dividends));

    EXPECT_EQ(report.mStatus, ReportStatus::NoData); // the input has no XML
    EXPECT_EQ(report.mRequestIds, std::vector<std::string>{request.mId});
    EXPECT_EQ(run.result().mStatus, OverallStatus::CompletedWithWarnings); // the payer warning
}

TEST(DividendPaymentsTest, PickingARowGivesTheFactsOfThatRowAndNoWarning) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer Y,TAX-Y,2 Main Street,FR,,,wikidata,manual,\n"
                    "XX0000000010,Payer X,TAX-X,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    DividendRun run{statement, catalogue, rates};

    // Option 1 is "Payer Y", the row 2 of the file.
    const auto rejections = run.answerFirst(PayerChoiceAnswer{.mPickedOption = 1});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.mLast.mDiagnostics.empty());
    ASSERT_EQ(run.mLast.mReport.mRows.size(), 1U);

    const auto& payer = run.mLast.mReport.mRows.at(0).mPayer;

    EXPECT_EQ(payer.mName, std::optional<std::string>{"Payer Y"});
    EXPECT_EQ(payer.mAddress, std::optional<std::string>{"2 Main Street"});
    EXPECT_EQ(payer.mCountry, std::optional<std::string>{"FR"});
    EXPECT_EQ(payer.mIdentificationNumber, std::optional<std::string>{"TAX-Y"});
    EXPECT_EQ(payer.mCatalogueRow->mRowNumber, 2U);
    EXPECT_EQ(run.mLast.mReport.mRows.at(0).mSourceCountry, std::optional<std::string>{"FR"});

    // The answer is listed as a used decision of the run.
    ASSERT_EQ(run.result().mDecisions.size(), 1U);
    EXPECT_TRUE(run.result().mDecisions.at(0).mUsed);
}

TEST(DividendPaymentsTest, SkippingTheChoiceGivesNoFactsAndTheSameWarningAsNotFound) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer Y,TAX-Y,2 Main Street,FR,,,wikidata,manual,\n"
                    "XX0000000010,Payer X,TAX-X,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    DividendRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(PayerChoiceAnswer{.mPickedOption = std::nullopt});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    ASSERT_EQ(run.mLast.mReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mReport.mRows.at(0).mPayer, IncomePayer{});
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::DividendPayerNotFound), 1U);
    ASSERT_EQ(run.result().mDecisions.size(), 1U);
    EXPECT_TRUE(run.result().mDecisions.at(0).mUsed);
}

TEST(DividendPaymentsTest, AChangedReferenceRowAsksThePayerChoiceAgain) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer Y,TAX-Y,2 Main Street,FR,,,wikidata,manual,\n"
                    "XX0000000010,Payer X,TAX-X,1 Main Street,DE,,,issuer,manual,\n");
    // The same rows, but "Payer Y" has moved to another address.
    const auto changedCatalogue =
        catalogueOf("XX0000000010,Payer Y,TAX-Y,9 New Street,FR,,,wikidata,manual,\n"
                    "XX0000000010,Payer X,TAX-X,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    DividendRun run{statement, catalogue, rates};

    // Option 1 is "Payer Y"; the answer is used while the rows are unchanged.
    const auto rejections = run.answerFirst(PayerChoiceAnswer{.mPickedOption = 1});

    ASSERT_TRUE(rejections.empty());
    ASSERT_TRUE(run.result().mRequests.empty());

    run.start(statement, changedCatalogue, rates);

    // The question data differs (another address), so the old answer does not apply: the
    // question is open again and the row has no payer facts.
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_EQ(run.mLast.mReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mReport.mRows.at(0).mPayer, IncomePayer{});
    EXPECT_EQ(countOf(run.mLast.mDiagnostics, ProcessingDiagnosticCode::DividendPayerNotFound), 1U);
    ASSERT_EQ(run.result().mDecisions.size(), 1U);
    EXPECT_FALSE(run.result().mDecisions.at(0).mUsed);
}

TEST(DividendPaymentsTest, TheSessionRejectsAPositionOutsideTheOptionsAndACancel) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer Y,TAX-Y,2 Main Street,FR,,,wikidata,manual,\n"
                    "XX0000000010,Payer X,TAX-X,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    DividendRun run{statement, catalogue, rates};

    // Two options exist, so the positions are 0 and 1.
    const auto outside = run.answerFirst(PayerChoiceAnswer{.mPickedOption = 2});
    const auto cancel = run.answerFirst(CancelAnswer{});

    ASSERT_EQ(outside.size(), 1U);
    EXPECT_EQ(outside.at(0).mReason, AnswerRejection::AnswerDoesNotFit);
    ASSERT_EQ(cancel.size(), 1U);
    EXPECT_EQ(cancel.at(0).mReason, AnswerRejection::AnswerDoesNotFit);
    // Nothing was recorded and the question is still open.
    EXPECT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_TRUE(run.result().mDecisions.empty());
}

TEST(DividendPaymentsTest, ASlovenianPayerHasNoRowAndIsAnExcludedItem) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer SI,12345678,1 Main Street,SI,,,issuer,manual,\n");
    OfficialRateTable rates; // no rate file: a Slovenian payer needs no rate
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer SI",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 100'000, // USD 10.00
                 .mTaxPaid = std::nullopt,
                 .mCurrency = Currency::USD,
                 .mTaxCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-20"),
                               .mSources = {source(3, "synthetic-div-2")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    DividendRun run{statement, catalogue, rates};

    EXPECT_TRUE(run.mLast.mReport.mRows.empty());
    EXPECT_TRUE(run.mLast.mUsedRates.empty());

    // One excluded item for the ISIN, with the sources of both payments and its warning.
    ASSERT_EQ(run.mLast.mExcludedItems.size(), 1U);

    const auto& item = run.mLast.mExcludedItems.at(0).mItem;

    EXPECT_EQ(item.mIsin, "XX0000000010");
    EXPECT_EQ(item.mName, "Synthetic Payer SI");
    EXPECT_EQ(item.mCategory, ExcludedCategory::Dividend);
    EXPECT_EQ(item.mReason, ExcludedReason::SlovenianTaxSettled);
    EXPECT_TRUE(item.isTaxRule());
    EXPECT_EQ(run.mLast.mExcludedItems.at(0).mSources.size(), 2U);

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::TaxCategoryNotReported);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mMessageKey,
              std::optional<MessageKey>{MessageKey::TaxCategoryNotReportedSlovenianTaxSettled});
    EXPECT_EQ(run.mLast.mExcludedItems.at(0).mProcessingDiagnostics, std::vector<std::size_t>{0});

    // In the result the item lists its warning, and Doh-Div has no data (no XML in the input).
    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);
    EXPECT_EQ(run.result().mExcludedItems.at(0).mDiagnosticIds.size(), 1U);
    EXPECT_EQ(dividendStatus(run.result()), ReportStatus::NoData);
}

TEST(DividendPaymentsTest, ASlovenianRowThatTheUserPicksIsExcludedToo) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer SI,12345678,1 Main Street,SI,,,issuer,manual,\n"
                    "XX0000000010,Payer FR,TAX-FR,2 Main Street,FR,,,wikidata,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    DividendRun run{statement, catalogue, rates};

    // Option 0 is "Payer FR", option 1 is "Payer SI".
    EXPECT_EQ(run.mLast.mReport.mRows.size(), 1U);

    const auto rejections = run.answerFirst(PayerChoiceAnswer{.mPickedOption = 1});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.mLast.mReport.mRows.empty());
    ASSERT_EQ(run.mLast.mExcludedItems.size(), 1U);
    EXPECT_EQ(run.mLast.mExcludedItems.at(0).mItem.mReason, ExcludedReason::SlovenianTaxSettled);
    EXPECT_TRUE(run.result().mRequests.empty());
}

TEST(DividendPaymentsTest, ASlovenianPayerKeepsTheDividendsOfOtherPayersInTheReport) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer SI,12345678,1 Main Street,SI,,,issuer,manual,\n"
                    "XX0000000028,Payer B,TAX-B,2 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {
        {
            .mName = "Synthetic Payer SI",
            .mIsin = "XX0000000010",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2025-03-14"),
                                   .mSources = {source(2, "synthetic-si-1")}},
                     .mGrossAmount = 50'000, // EUR 5.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Payer B",
            .mIsin = "XX0000000028",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2025-03-14"),
                                   .mSources = {source(3, "synthetic-b-1")}},
                     .mGrossAmount = 20'000, // EUR 2.00
                     .mTaxPaid = 3'000,      // EUR 0.30
                     .mCurrency = Currency::EUR,
                     .mTaxCurrency = Currency::EUR},
                },
        },
    };

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    ASSERT_EQ(result.mReport.mRows.size(), 1U);
    EXPECT_EQ(result.mReport.mRows.at(0).mIsin, "XX0000000028");
    EXPECT_EQ(result.mReport.mRows.at(0).mPayment.mInstrumentIndex, std::optional<std::size_t>{1});
    ASSERT_EQ(result.mExcludedItems.size(), 1U);
    EXPECT_EQ(result.mExcludedItems.at(0).mItem.mIsin, "XX0000000010");
}

// --- Notices ---

TEST(DividendPaymentsTest, ForeignTaxAboveZeroGivesOneEvidenceNoticeForTheRun) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 7'500,      // EUR 0.75
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-20"),
                               .mSources = {source(3, "synthetic-div-2")}},
                 .mGrossAmount = 60'000, // EUR 6.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-09-20"),
                               .mSources = {source(4, "synthetic-div-3")}},
                 .mGrossAmount = 70'000, // EUR 7.00
                 .mTaxPaid = 9'000,      // EUR 0.90
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    // One notice for the run, with the sources of the two payments that paid tax.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& notice = result.mDiagnostics.at(0);

    EXPECT_EQ(notice.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(notice.mCode, ProcessingDiagnosticCode::ForeignTaxEvidenceRequired);
    EXPECT_EQ(notice.mMessageKey,
              std::optional<MessageKey>{MessageKey::ForeignTaxEvidenceRequired});
    EXPECT_EQ(notice.mContext.mAffectedReports, std::vector<ReportType>{ReportType::Dividends});
    EXPECT_EQ(notice.mContext.mIsin, std::nullopt);
    ASSERT_EQ(notice.mContext.mSources.size(), 2U);
    EXPECT_EQ(notice.mContext.mSources.at(0).mSourceRow, 2U);
    EXPECT_EQ(notice.mContext.mSources.at(1).mSourceRow, 4U);
}

TEST(DividendPaymentsTest, NoForeignTaxGivesNoEvidenceNotice) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-06-20"),
                               .mSources = {source(3, "synthetic-div-2")}},
                 .mGrossAmount = 60'000, // EUR 6.00
                 .mTaxPaid = 0,
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    ASSERT_EQ(result.mReport.mRows.size(), 1U);
    EXPECT_EQ(countOf(result.mDiagnostics, ProcessingDiagnosticCode::ForeignTaxEvidenceRequired),
              0U);
}

TEST(DividendPaymentsTest, AReliefStatementOfThePayerCountryDoesNotMakeATreatyClaim) {
    // The synthetic catalogue has a relief statement for BE, the country of this payer.
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,BE,,,issuer,manual,\n");
    OfficialRateTable rates;
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {{
        .mName = "Synthetic Payer A",
        .mIsin = "XX0000000010",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2025-03-14"),
                               .mSources = {source(2, "synthetic-div-1")}},
                 .mGrossAmount = 50'000, // EUR 5.00
                 .mTaxPaid = 7'500,      // EUR 0.75
                 .mCurrency = Currency::EUR,
                 .mTaxCurrency = Currency::EUR},
            },
    }};

    orderChronologically(statement);
    RunDecisions decisions;

    ASSERT_EQ(catalogue.reliefStatements("BE").size(), 1U);

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    ASSERT_EQ(result.mReport.mRows.size(), 1U);
    EXPECT_EQ(result.mReport.mRows.at(0).mTreatyClaim, std::nullopt);
}

TEST(DividendPaymentsTest, TwoPaymentsOnOneCurrencyAndDayUseOneRate) {
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n"
                    "XX0000000028,Payer B,TAX-B,2 Main Street,FR,,,issuer,manual,\n");
    OfficialRateTable rates;
    addFile2025(rates);
    addRate(rates, "USD", "2025-03-14", 108'800'000, 3); // 1 EUR = 1.0880 USD
    MergedStatement statement;
    statement.mPresentation.mDividendInstruments = {
        {
            .mName = "Synthetic Payer A",
            .mIsin = "XX0000000010",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2025-03-14"),
                                   .mSources = {source(2, "synthetic-a-1")}},
                     .mGrossAmount = 100'000, // USD 10.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::USD,
                     .mTaxCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Payer B",
            .mIsin = "XX0000000028",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2025-03-14"),
                                   .mSources = {source(3, "synthetic-b-1")}},
                     .mGrossAmount = 200'000, // USD 20.00
                     .mTaxPaid = 0,
                     .mCurrency = Currency::USD,
                     .mTaxCurrency = Currency::EUR},
                },
        },
    };

    orderChronologically(statement);
    RunDecisions decisions;

    const auto result =
        prepareDividendPayments(statement, ledger2025(), catalogue, rates, decisions);

    ASSERT_EQ(result.mReport.mRows.size(), 2U);
    EXPECT_EQ(result.mReport.mRows.at(0).mGrossIncome, 91'912);  // 10 / 1.088 = 9.1912
    EXPECT_EQ(result.mReport.mRows.at(1).mGrossIncome, 183'824); // 20 / 1.088 = 18.38235294
    EXPECT_EQ(result.mUsedRates.size(), 1U);
}

} // namespace
