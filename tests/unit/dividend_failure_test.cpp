#include "processors/dividend_payments.hpp"
#include "processors/failure_resolution_stage.hpp"
#include "processors/income_payers.hpp"

#include "taxbroker/currency.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace taxbroker;

// All payers, ISINs, dates, amounts and rates in this test suite are synthetic. The synthetic
// ISINs use the prefix XX and a valid check digit. Money has 4 decimals, so 50'000 is EUR 5.00.

constexpr std::string_view kPayersHeader =
    "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
    "source_revision\n";
constexpr std::string_view kReliefHeader = "country,income,statement,source,source_revision\n";
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
    const std::string relief = std::string{kReliefHeader} + "BE,dividend,Treaty text,manual,\n";
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

/// Both payers are German, so no payer is Slovenian.
constexpr std::string_view kGermanPayers =
    "XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n"
    "XX0000000028,Payer B,TAX-B,2 Main Street,DE,,,issuer,manual,\n";

/// One dividend of one instrument.
DividendInstrument dividendOf(std::string aName,
                              Isin aIsin,
                              std::string_view aDate,
                              std::size_t aRow,
                              Money aGross,
                              Currency aCurrency,
                              Money aTax,
                              Currency aTaxCurrency) {
    return DividendInstrument{
        .mName = std::move(aName),
        .mIsin = std::move(aIsin),
        .mTransactions = {{.mMetadata = {.mTaxDate = day(aDate),
                                         .mSources = {source(aRow, "synthetic-div")}},
                           .mGrossAmount = aGross,
                           .mTaxPaid = aTax,
                           .mCurrency = aCurrency,
                           .mTaxCurrency = aTaxCurrency}},
    };
}

/// Runs the dividend stage and the Doh-Div failure stage in a session, as the report command does
/// once it makes Doh-Div: the rows of the excluded ISINs are removed after the answer. A form with
/// rows gets a placeholder XML, so a status of `generated` shows that nothing blocks it.
struct DivRun {
    DecisionSession mSession;
    DividendPaymentsResult mLast;
    std::vector<Isin> mExcluded;

    DivRun(const MergedStatement& aStatement,
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
            mExcluded = FailureResolutionStage::resolve(input, aDecisions, kDividendsFailure);
            excludeDividends(mLast.mReport, mExcluded);

            if (!mLast.mReport.mRows.empty())
            {
                input.mReports[static_cast<std::size_t>(ReportType::Dividends)].mXml =
                    "<Envelope/>";
            }

            return input;
        });
    }

    [[nodiscard]] const ApplicationResult& result() const {
        return mSession.result();
    }

    [[nodiscard]] ReportStatus divStatus() const {
        return result().report(ReportType::Dividends).mStatus;
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

/// A statement of the given dividends, in date order.
MergedStatement statementOf(std::vector<DividendInstrument> aInstruments) {
    MergedStatement statement;

    statement.mPresentation.mDividendInstruments = std::move(aInstruments);
    orderChronologically(statement);

    return statement;
}

/// The rate table of 2025 with the file loaded and no USD rate.
OfficialRateTable fileWithoutUsd() {
    OfficialRateTable rates;

    addFile2025(rates);

    return rates;
}

TEST(DividendFailureTest, ADividendWithoutARateIsAnErrorOfItsIsinAndAsksTheFailureQuestion) {
    const auto catalogue = catalogueOf(kGermanPayers);
    const auto rates = fileWithoutUsd();
    const auto statement = statementOf({dividendOf("Synthetic Payer A",
                                                   "XX0000000010",
                                                   "2025-03-14",
                                                   2,
                                                   100'000, // USD 10.00
                                                   Currency::USD,
                                                   0,
                                                   Currency::EUR),
                                        dividendOf("Synthetic Payer B",
                                                   "XX0000000028",
                                                   "2025-03-15",
                                                   3,
                                                   50'000, // EUR 5.00
                                                   Currency::EUR,
                                                   0,
                                                   Currency::EUR)});
    DivRun run{statement, catalogue, rates};

    // One error, made for the dividend of Payer A: its ISIN, Doh-Div only.
    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);

    const auto& error = run.mLast.mDiagnostics.at(0);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(error.mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_EQ(error.mContext.mAffectedReports, std::vector<ReportType>{ReportType::Dividends});
    EXPECT_EQ(error.mContext.mSources.size(), 1U);

    // The question lists Payer A with the reason of its error; Doh-Div waits for the answer.
    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto& request = run.result().mRequests.at(0);

    EXPECT_EQ(request.mType, RequestType::DividendsFailureResolutionRequired);
    ASSERT_EQ(request.mInvestments.size(), 1U);
    EXPECT_EQ(request.mInvestments.at(0).mIsin, "XX0000000010");
    EXPECT_EQ(request.mInvestments.at(0).mName, "Synthetic Payer A");
    EXPECT_EQ(
        request.mInvestments.at(0).mReasonCodes,
        std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::MissingExchangeRate});
    EXPECT_EQ(run.divStatus(), ReportStatus::NeedsInput);

    // Payer B has its row: EUR 5.00.
    ASSERT_EQ(run.mLast.mReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mReport.mRows.at(0).mIsin, "XX0000000028");
    EXPECT_EQ(run.mLast.mReport.mRows.at(0).mGrossIncome, 50'000);
}

TEST(DividendFailureTest, ExcludingTheIsinKeepsTheOtherDividends) {
    const auto catalogue = catalogueOf(kGermanPayers);
    const auto rates = fileWithoutUsd();
    const auto statement = statementOf({dividendOf("Synthetic Payer A",
                                                   "XX0000000010",
                                                   "2025-03-14",
                                                   2,
                                                   100'000, // USD 10.00
                                                   Currency::USD,
                                                   0,
                                                   Currency::EUR),
                                        dividendOf("Synthetic Payer B",
                                                   "XX0000000028",
                                                   "2025-03-15",
                                                   3,
                                                   50'000, // EUR 5.00
                                                   Currency::EUR,
                                                   0,
                                                   Currency::EUR)});
    DivRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(kExclude);

    EXPECT_TRUE(rejections.empty());
    EXPECT_EQ(run.mExcluded, std::vector<Isin>{"XX0000000010"});
    ASSERT_EQ(run.mLast.mReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mReport.mRows.at(0).mIsin, "XX0000000028");
    EXPECT_EQ(run.mLast.mReport.mRows.at(0).mGrossIncome, 50'000);

    // The error stays visible and no longer blocks; the form is made.
    const auto& div = run.result().report(ReportType::Dividends);

    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);
    EXPECT_FALSE(run.result().mDiagnostics.at(0).mBlocking);
    EXPECT_EQ(div.mStatus, ReportStatus::Generated);
    EXPECT_EQ(div.mFailureResolution, FailureResolution::ExcludeFailedInstruments);
    ASSERT_EQ(div.mExcludedInvestments.size(), 1U);
    EXPECT_EQ(div.mExcludedInvestments.at(0).mIsin, "XX0000000010");
    EXPECT_EQ(div.mExcludedInvestments.at(0).mDiagnosticIds,
              std::vector<std::string>{"diagnostic-0001"});
    EXPECT_EQ(run.result().mStatus, OverallStatus::CompletedWithWarnings);
}

TEST(DividendFailureTest, ExcludingTheOnlyIsinLeavesNoDataAndNoXml) {
    const auto catalogue = catalogueOf(kGermanPayers);
    const auto rates = fileWithoutUsd();
    const auto statement = statementOf({dividendOf("Synthetic Payer A",
                                                   "XX0000000010",
                                                   "2025-03-14",
                                                   2,
                                                   100'000, // USD 10.00
                                                   Currency::USD,
                                                   0,
                                                   Currency::EUR)});
    DivRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(kExclude);

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.mLast.mReport.mRows.empty());
    EXPECT_EQ(run.divStatus(), ReportStatus::NoData);
    EXPECT_FALSE(run.result().report(ReportType::Dividends).mXml.has_value());
    EXPECT_EQ(run.result().report(ReportType::Dividends).mExcludedInvestments.size(), 1U);
}

TEST(DividendFailureTest, FailingTheReportKeepsTheErrorBlockingAndExcludesNothing) {
    const auto catalogue = catalogueOf(kGermanPayers);
    const auto rates = fileWithoutUsd();
    const auto statement = statementOf({dividendOf("Synthetic Payer A",
                                                   "XX0000000010",
                                                   "2025-03-14",
                                                   2,
                                                   100'000, // USD 10.00
                                                   Currency::USD,
                                                   0,
                                                   Currency::EUR),
                                        dividendOf("Synthetic Payer B",
                                                   "XX0000000028",
                                                   "2025-03-15",
                                                   3,
                                                   50'000, // EUR 5.00
                                                   Currency::EUR,
                                                   0,
                                                   Currency::EUR)});
    DivRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(kFailReport);
    const auto& div = run.result().report(ReportType::Dividends);

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.mExcluded.empty());
    EXPECT_EQ(div.mStatus, ReportStatus::Failed);
    EXPECT_EQ(div.mFailureResolution, FailureResolution::FailReport);
    EXPECT_TRUE(div.mExcludedInvestments.empty());
    ASSERT_EQ(run.result().mDiagnostics.size(), 1U);
    EXPECT_TRUE(run.result().mDiagnostics.at(0).mBlocking);
    EXPECT_FALSE(div.mXml.has_value());
}

TEST(DividendFailureTest, CancellingTheQuestionFailsDohDivWithoutAResolution) {
    const auto catalogue = catalogueOf(kGermanPayers);
    const auto rates = fileWithoutUsd();
    const auto statement = statementOf({dividendOf("Synthetic Payer A",
                                                   "XX0000000010",
                                                   "2025-03-14",
                                                   2,
                                                   100'000, // USD 10.00
                                                   Currency::USD,
                                                   0,
                                                   Currency::EUR)});
    DivRun run{statement, catalogue, rates};

    const auto rejections = run.answerFirst(CancelAnswer{});
    const auto& div = run.result().report(ReportType::Dividends);

    EXPECT_TRUE(rejections.empty());
    EXPECT_EQ(div.mStatus, ReportStatus::Failed);
    EXPECT_EQ(div.mFailureResolution, std::nullopt);
    EXPECT_TRUE(div.mExcludedInvestments.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
}

TEST(DividendFailureTest, AChangedListOfIsinsNeedsANewAnswer) {
    const auto catalogue = catalogueOf(kGermanPayers);
    const auto rates = fileWithoutUsd();
    const auto first = statementOf({dividendOf("Synthetic Payer A",
                                               "XX0000000010",
                                               "2025-03-14",
                                               2,
                                               100'000, // USD 10.00
                                               Currency::USD,
                                               0,
                                               Currency::EUR)});
    const auto second = statementOf({dividendOf("Synthetic Payer A",
                                                "XX0000000010",
                                                "2025-03-14",
                                                2,
                                                100'000, // USD 10.00
                                                Currency::USD,
                                                0,
                                                Currency::EUR),
                                     dividendOf("Synthetic Payer B",
                                                "XX0000000028",
                                                "2025-03-15",
                                                3,
                                                50'000, // USD 5.00
                                                Currency::USD,
                                                0,
                                                Currency::EUR)});
    DivRun run{first, catalogue, rates};

    static_cast<void>(run.answerFirst(kExclude));
    EXPECT_EQ(run.mExcluded, std::vector<Isin>{"XX0000000010"});

    // Payer B now fails too: the list [A, B] is not the answered list [A].
    run.start(second, catalogue, rates);

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_EQ(run.result().mRequests.at(0).mInvestments.size(), 2U);
    EXPECT_EQ(run.result().mRequests.at(0).mInvestments.at(0).mIsin, "XX0000000010");
    EXPECT_EQ(run.result().mRequests.at(0).mInvestments.at(1).mIsin, "XX0000000028");
    EXPECT_TRUE(run.mExcluded.empty());
    EXPECT_EQ(run.divStatus(), ReportStatus::NeedsInput);
    EXPECT_FALSE(run.result().report(ReportType::Dividends).mFailureResolution.has_value());
}

TEST(DividendFailureTest, TwoDividendsWithoutARateOnOneDayGiveOneErrorEach) {
    const auto catalogue = catalogueOf(kGermanPayers);
    const auto rates = fileWithoutUsd();
    const auto statement = statementOf({dividendOf("Synthetic Payer A",
                                                   "XX0000000010",
                                                   "2025-03-14",
                                                   2,
                                                   100'000, // USD 10.00
                                                   Currency::USD,
                                                   0,
                                                   Currency::EUR),
                                        dividendOf("Synthetic Payer B",
                                                   "XX0000000028",
                                                   "2025-03-14",
                                                   3,
                                                   100'000, // USD 10.00
                                                   Currency::USD,
                                                   0,
                                                   Currency::EUR)});
    DivRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 2U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_EQ(run.mLast.mDiagnostics.at(1).mContext.mIsin, std::optional<Isin>{"XX0000000028"});
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_EQ(run.result().mRequests.at(0).mInvestments.size(), 2U);
    EXPECT_EQ(run.result().mRequests.at(0).mInvestments.at(0).mIsin, "XX0000000010");
    EXPECT_EQ(run.result().mRequests.at(0).mInvestments.at(1).mIsin, "XX0000000028");
    EXPECT_TRUE(run.mLast.mReport.mRows.empty());
}

TEST(DividendFailureTest, AForeignTaxWithoutARateIsOneErrorOfTheDividendsIsin) {
    const auto catalogue = catalogueOf(kGermanPayers);
    const auto rates = fileWithoutUsd();
    const auto statement = statementOf({dividendOf("Synthetic Payer A",
                                                   "XX0000000010",
                                                   "2025-03-14",
                                                   2,
                                                   100'000, // EUR 10.00
                                                   Currency::EUR,
                                                   15'000, // USD 1.50
                                                   Currency::USD)});
    DivRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_TRUE(run.mLast.mReport.mRows.empty());
}

TEST(DividendFailureTest, AGrossAndATaxInTheSameMissingCurrencyAreOneError) {
    const auto catalogue = catalogueOf(kGermanPayers);
    const auto rates = fileWithoutUsd();
    const auto statement = statementOf({dividendOf("Synthetic Payer A",
                                                   "XX0000000010",
                                                   "2025-03-14",
                                                   2,
                                                   100'000, // USD 10.00
                                                   Currency::USD,
                                                   15'000, // USD 1.50
                                                   Currency::USD)});
    DivRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    EXPECT_TRUE(run.mLast.mReport.mRows.empty());
}

TEST(DividendFailureTest, ADividendThatDoesNotFitAfterTheConversionBelongsToItsIsin) {
    const auto catalogue = catalogueOf(kGermanPayers);
    auto rates = fileWithoutUsd();

    addRate(rates, "USD", "2025-03-14", 1, 3); // 1 EUR = 0.00000001 USD

    // USD 900'000'000'000'000.0000 divided by 0.00000001 is 9e22 EUR: no int64.
    const auto statement = statementOf({dividendOf("Synthetic Payer A",
                                                   "XX0000000010",
                                                   "2025-03-14",
                                                   2,
                                                   9'000'000'000'000'000'000,
                                                   Currency::USD,
                                                   0,
                                                   Currency::EUR)});
    DivRun run{statement, catalogue, rates};

    ASSERT_EQ(run.mLast.mDiagnostics.size(), 1U);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(run.mLast.mDiagnostics.at(0).mContext.mIsin, std::optional<Isin>{"XX0000000010"});
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests.at(0).mType, RequestType::DividendsFailureResolutionRequired);
    EXPECT_EQ(run.result().mRequests.at(0).mInvestments.at(0).mIsin, "XX0000000010");
}

TEST(DividendFailureTest, TheConverterGivesNoAmountForACurrencyWithoutARate) {
    // The callers check the rate first and make the error; the converter only refuses.
    const OfficialRateSelection selection;
    std::vector<ProcessingDiagnostic> diagnostics;
    IncomeEurConverter converter{selection, diagnostics};
    const EventMetadata metadata{.mTaxDate = day("2025-03-14"),
                                 .mSources = {source(2, "synthetic-div")}};

    const auto eur = converter.toEur(ReportType::Dividends,
                                     metadata,
                                     "Synthetic Payer A",
                                     Isin{"XX0000000010"},
                                     100'000, // USD 10.00
                                     Currency::USD,
                                     "gross");

    EXPECT_FALSE(eur.has_value());
    EXPECT_TRUE(diagnostics.empty());
}

TEST(DividendFailureTest, ExcludingTheIsinAlsoDropsItsOpenPayerChoice) {
    // Two reference rows disagree on the payer of Payer A: a payer choice, which never blocks.
    const auto catalogue =
        catalogueOf("XX0000000010,Payer A,TAX-A,1 Main Street,DE,,,issuer,manual,\n"
                    "XX0000000010,Payer A2,TAX-A2,3 Main Street,FR,,,issuer,manual,\n"
                    "XX0000000028,Payer B,TAX-B,2 Main Street,DE,,,issuer,manual,\n");
    const auto rates = fileWithoutUsd();
    const auto statement = statementOf({dividendOf("Synthetic Payer A",
                                                   "XX0000000010",
                                                   "2025-03-14",
                                                   2,
                                                   100'000, // USD 10.00
                                                   Currency::USD,
                                                   0,
                                                   Currency::EUR),
                                        dividendOf("Synthetic Payer B",
                                                   "XX0000000028",
                                                   "2025-03-15",
                                                   3,
                                                   50'000, // EUR 5.00
                                                   Currency::EUR,
                                                   0,
                                                   Currency::EUR)});
    DivRun run{statement, catalogue, rates};

    // The payer choice and the failure question are both open.
    ASSERT_EQ(run.result().mRequests.size(), 2U);

    const auto failureRequest =
        std::find_if(run.result().mRequests.begin(),
                     run.result().mRequests.end(),
                     [](const DecisionRequest& aRequest) {
                         return aRequest.mType == RequestType::DividendsFailureResolutionRequired;
                     });

    ASSERT_NE(failureRequest, run.result().mRequests.end());

    const auto rejections = run.mSession.answer({DecisionAnswer{
        .mRequestId = failureRequest->mId,
        .mRunRevision = run.result().mRunRevision,
        .mValue = kExclude,
    }});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_EQ(run.divStatus(), ReportStatus::Generated);
    ASSERT_EQ(run.mLast.mReport.mRows.size(), 1U);
    EXPECT_EQ(run.mLast.mReport.mRows.at(0).mIsin, "XX0000000028");
}

} // namespace
