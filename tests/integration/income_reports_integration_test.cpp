#include "parsers/traderepublic_parser.hpp"
#include "processors/income_reports.hpp"
#include "processors/tax_processor.hpp"
#include "taxbroker/currency.hpp"
#include "taxbroker/decision_session.hpp"
#include "taxbroker/official_rates.hpp"
#include "taxbroker/reference_catalogue.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

// All names, ISINs, dates and amounts here are synthetic. Money is stored with 4 decimals, so
// 100'000 is EUR 10.00 and 15'000 is EUR 1.50. The synthetic ISINs use the prefix XX, which is
// never in the built-in payer file, so no payer is found. No rate file is loaded, so a payment in
// another currency has no official rate.

/// Writes the synthetic Trade Republic rows of one scenario to a CSV file and removes it again.
class TemporaryCsv {
  public:
    explicit TemporaryCsv(std::initializer_list<std::string_view> aRows) {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();

        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_income_synthetic_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create the synthetic income directory"};
        }

        mPath = mDirectory / "history.csv";

        std::ofstream output{mPath};

        output << "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,"
                  "amount,fee,tax,currency,original_amount,original_currency,fx_rate,description,"
                  "transaction_id,counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

        for (const auto row : aRows)
        {
            output << row << '\n';
        }

        output.close();

        if (!output)
        {
            throw std::runtime_error{"Unable to write the synthetic income CSV"};
        }
    }

    TemporaryCsv(const TemporaryCsv&) = delete;
    TemporaryCsv& operator=(const TemporaryCsv&) = delete;

    ~TemporaryCsv() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return mPath;
    }

  private:
    std::filesystem::path mDirectory;
    std::filesystem::path mPath;
};

/// The income stages on the rows of one file, in a decision session so a test can answer.
class IncomeRun {
  public:
    IncomeRun(const TemporaryCsv& aCsv, int aYear) : IncomeRun(aCsv, aYear, OfficialRateTable{}) {}

    IncomeRun(const TemporaryCsv& aCsv, int aYear, OfficialRateTable aRates)
        : mRates(std::move(aRates)) {
        tr::TradeRepublicParser parser;
        std::vector<StatementMergeInput> inputs{{0, parser.parse(aCsv.path())}};

        mPrepared = TaxProcessor::prepareReportingYear(
            {aYear, DeterministicStatementMerger{}.merge(std::move(inputs))});

        mSession.start([this, aYear](RunDecisions& aDecisions) {
            ApplicationResultInput input{
                .mReportingYear = aYear,
                .mMergeDiagnostics = mPrepared.mMerged.mDiagnostics,
                .mProcessingDiagnostics = mPrepared.mDiagnostics,
            };

            mReports = prepareIncomeReports(input,
                                            mPrepared.mMerged.mStatement,
                                            mPrepared.mLedger.value(),
                                            referenceCatalogue(),
                                            mRates,
                                            aDecisions);

            return input;
        });
    }

    [[nodiscard]] const ApplicationResult& result() const {
        return mSession.result();
    }

    [[nodiscard]] const IncomeReports& reports() const {
        return mReports;
    }

    [[nodiscard]] ReportStatus status(ReportType aType) const {
        return result().report(aType).mStatus;
    }

    /// Answers the first open request and returns the rejections.
    std::vector<RejectedAnswer> answerFirst(AnswerValue aAnswer) {
        return mSession.answer({DecisionAnswer{
            .mRequestId = result().mRequests.at(0).mId,
            .mRunRevision = result().mRunRevision,
            .mValue = std::move(aAnswer),
        }});
    }

    /// The diagnostics with this code name, such as `dividend_payer_not_found`.
    [[nodiscard]] std::vector<const ApplicationDiagnostic*>
    diagnosticsOf(ProcessingDiagnosticCode aCode) const {
        std::vector<const ApplicationDiagnostic*> found;

        for (const auto& diagnostic : result().mDiagnostics)
        {
            const auto* code = std::get_if<ProcessingDiagnosticCode>(&diagnostic.mCode);

            if (code != nullptr && *code == aCode)
            {
                found.push_back(&diagnostic);
            }
        }

        return found;
    }

  private:
    TaxProcessingResult mPrepared;
    OfficialRateTable mRates;
    DecisionSession mSession;
    IncomeReports mReports;
};

/// Marks the rate file of 2024 as loaded, from 2 January to 31 December.
void addFile2024(OfficialRateTable& aTable) {
    aTable.mFiles.push_back(
        OfficialRateFile{.mSourceFile = SourceFilename::fromPath("exr_2024.csv"),
                         .mYear = 2024,
                         .mFirstDate = *parseCalendarDate("2024-01-02"),
                         .mLastDate = *parseCalendarDate("2024-12-31"),
                         .mRateCount = 3,
                         .mOrigin = OfficialRateOrigin::Supplied});
}

/// Adds one rate; aValue is foreign units per 1 EUR at 8 decimals.
void addRate(OfficialRateTable& aTable,
             std::string_view aCode,
             std::string_view aDate,
             ExchangeRate aValue,
             std::size_t aRow) {
    const auto date = *parseCalendarDate(aDate);

    aTable.mRates[{std::string{aCode}, date}] =
        OfficialRate{.mCurrencyCode = std::string{aCode},
                     .mObservationDate = date,
                     .mValue = aValue,
                     .mSourceFile = SourceFilename::fromPath("exr_2024.csv"),
                     .mSourceRow = aRow,
                     .mSource = OfficialRateSource::EcbReferenceRateFile,
                     .mOrigin = OfficialRateOrigin::Supplied};
}

const AnswerValue kExclude = FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments};
const AnswerValue kFailReport = FailureResolutionAnswer{FailureResolution::FailReport};

TEST(IncomeReportsIntegrationTest, ADividendWithoutAReferenceRowIsAReportedRowWithEmptyPayerFacts) {
    // XX0000000001: EUR 10.00, tax column blank. XX0000000002: EUR 10.00, tax EUR 1.50.
    const TemporaryCsv csv{
        R"(2024-04-01T09:00:00Z,2024-04-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share A,XX0000000001,,,10,,,EUR,,,,,synthetic-dividend-1,,,,)",
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share B,XX0000000002,,,10,,-1.50,EUR,,,,,synthetic-dividend-2,,,,)",
    };

    const IncomeRun run{csv, 2024};

    const auto& rows = run.reports().mDividends.mRows;

    ASSERT_EQ(rows.size(), 2U);

    EXPECT_EQ(rows[0].mIsin, "XX0000000001");
    EXPECT_EQ(rows[0].mDate, *parseCalendarDate("2024-04-01"));
    EXPECT_EQ(rows[0].mGrossIncome, 100'000); // EUR 10.00
    EXPECT_EQ(rows[0].mWithholding.mForeignTax, 0);
    EXPECT_TRUE(rows[0].mWithholding.mBlankInExport);
    EXPECT_EQ(rows[0].mPayer, IncomePayer{}); // not in the reference files: empty facts
    EXPECT_FALSE(rows[0].mSourceCountry.has_value());
    EXPECT_FALSE(rows[0].mTreatyClaim.has_value());

    EXPECT_EQ(rows[1].mIsin, "XX0000000002");
    EXPECT_EQ(rows[1].mGrossIncome, 100'000);
    EXPECT_EQ(rows[1].mWithholding.mForeignTax, 15'000); // EUR 1.50
    EXPECT_FALSE(rows[1].mWithholding.mBlankInExport);

    // One warning per ISIN tells the user to complete the payer in eDavki.
    const auto warnings = run.diagnosticsOf(ProcessingDiagnosticCode::DividendPayerNotFound);

    ASSERT_EQ(warnings.size(), 2U);
    EXPECT_EQ(warnings[0]->mIsin, "XX0000000001");
    EXPECT_EQ(warnings[1]->mIsin, "XX0000000002");
    EXPECT_NE(run.status(ReportType::Dividends), ReportStatus::Failed);
    EXPECT_NE(run.status(ReportType::Dividends), ReportStatus::NeedsInput);
}

TEST(IncomeReportsIntegrationTest, OnlyPaymentsReceivedInTheYearAreRows) {
    // Amounts 1, 2, 3 and 4 on the last days of 2023 and the first days of 2025 and in 2024.
    const TemporaryCsv csv{
        R"(2023-12-29T09:00:00Z,2023-12-29,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,1,,,EUR,,,,,synthetic-dividend-1,,,,)",
        R"(2024-01-02T09:00:00Z,2024-01-02,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,2,,,EUR,,,,,synthetic-dividend-2,,,,)",
        R"(2024-12-31T09:00:00Z,2024-12-31,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,3,,,EUR,,,,,synthetic-dividend-3,,,,)",
        R"(2025-01-02T09:00:00Z,2025-01-02,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,4,,,EUR,,,,,synthetic-dividend-4,,,,)",
    };

    const IncomeRun run{csv, 2024};

    const auto& rows = run.reports().mDividends.mRows;

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].mDate, *parseCalendarDate("2024-01-02"));
    EXPECT_EQ(rows[0].mGrossIncome, 20'000); // EUR 2.00
    EXPECT_EQ(rows[1].mDate, *parseCalendarDate("2024-12-31"));
    EXPECT_EQ(rows[1].mGrossIncome, 30'000); // EUR 3.00
}

TEST(IncomeReportsIntegrationTest, DividendAndCouponRatesAreOneListByDateThenCode) {
    // Two USD dividends on 2024-03-15 and 2024-04-02 and a CHF coupon on 2024-04-02. The rates are
    // USD 1.25 and CHF 0.80 per EUR, so USD 10.00 is EUR 8.00 and CHF 8.00 is EUR 10.00.
    const TemporaryCsv csv{
        R"(2024-03-15T09:00:00Z,2024-03-15,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share A,XX0000000001,,,9,,,EUR,10,USD,0.900000,,synthetic-dividend-1,,,,)",
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share B,XX0000000002,,,9,,,EUR,10,USD,0.900000,,synthetic-dividend-2,,,,)",
        R"(2024-04-02T10:00:00Z,2024-04-02,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond,XX0000000003,,,9,,,EUR,8,CHF,1.100000,Synthetic bond interest,synthetic-coupon-1,,,,)",
    };

    OfficialRateTable rates;

    addFile2024(rates);
    addRate(rates, "USD", "2024-03-15", 125'000'000, 2);
    addRate(rates, "USD", "2024-04-02", 125'000'000, 3);
    addRate(rates, "CHF", "2024-04-02", 80'000'000, 4);

    const IncomeRun run{csv, 2024, std::move(rates)};

    ASSERT_EQ(run.reports().mDividends.mRows.size(), 2U);
    EXPECT_EQ(run.reports().mDividends.mRows[0].mGrossIncome, 80'000); // USD 10.00 / 1.25
    EXPECT_EQ(run.reports().mDividends.mRows[1].mGrossIncome, 80'000);
    ASSERT_EQ(run.reports().mOther.mRows.size(), 1U);
    EXPECT_EQ(run.reports().mOther.mRows[0].mGrossInterest, 100'000); // CHF 8.00 / 0.80

    // USD on 2024-04-02 is used by a dividend only; CHF on that day by the coupon only. The list
    // is by date, then currency code, with one entry per currency and day.
    const auto& used = run.reports().mUsedRates;

    ASSERT_EQ(used.size(), 3U);
    EXPECT_EQ(used[0].mCurrency, Currency::USD);
    EXPECT_EQ(used[0].mTaxDate, *parseCalendarDate("2024-03-15"));
    EXPECT_EQ(used[1].mCurrency, Currency::CHF);
    EXPECT_EQ(used[1].mTaxDate, *parseCalendarDate("2024-04-02"));
    EXPECT_EQ(used[2].mCurrency, Currency::USD);
    EXPECT_EQ(used[2].mTaxDate, *parseCalendarDate("2024-04-02"));
}

TEST(IncomeReportsIntegrationTest, InterestIsSortedIntoDhoAndOneObrWithBothSubtypes) {
    // Trade Republic is a broker before 6 December 2023, so the interest of 2023-11-01 is cash
    // interest (Doh-Obr); from that date it is a bank, so the interest of 2023-12-29 is deposit
    // interest (Doh-DHO). The coupon of XX0000000003 is a Doh-Obr row.
    const TemporaryCsv csv{
        R"(2023-08-01T10:00:00Z,2023-08-01,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Green Bond,XX0000000003,,,1.80,,-0.20,EUR,,,,Synthetic bond interest,synthetic-coupon-1,,,,)",
        R"(2023-11-01T10:00:00Z,2023-11-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,1.23,,,EUR,,,,Synthetic broker interest,synthetic-interest-1,,,,)",
        R"(2023-12-29T10:00:00Z,2023-12-29,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,2.00,,-0.50,EUR,,,,Synthetic bank interest,synthetic-interest-2,,,,)",
    };

    IncomeRun run{csv, 2023};

    // Doh-Obr: the coupon, then the cash interest, in date order.
    const auto& obrRows = run.reports().mOther.mRows;

    ASSERT_EQ(obrRows.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<BondCoupon>(obrRows[0].mKind));
    EXPECT_EQ(std::get<BondCoupon>(obrRows[0].mKind).mIsin, "XX0000000003");
    EXPECT_EQ(obrRows[0].mDate, *parseCalendarDate("2023-08-01"));
    EXPECT_EQ(obrRows[0].mGrossInterest, 18'000);          // EUR 1.80
    EXPECT_EQ(obrRows[0].mWithholding.mForeignTax, 2'000); // EUR 0.20
    EXPECT_TRUE(std::holds_alternative<BrokerCashInterest>(obrRows[1].mKind));
    EXPECT_EQ(obrRows[1].mDate, *parseCalendarDate("2023-11-01"));
    EXPECT_EQ(obrRows[1].mGrossInterest, 12'300); // EUR 1.23

    // Doh-DHO: EUR 2.00 is at most EUR 1,000, so the allowance question comes first and the
    // form has no rows yet.
    EXPECT_TRUE(run.reports().mDeposit.mRows.empty());
    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto* question =
        std::get_if<DepositInterestTotalQuestion>(&run.result().mRequests[0].mData);

    ASSERT_NE(question, nullptr);
    EXPECT_EQ(question->mYear, 2023);
    EXPECT_EQ(question->mTotal, 20'000); // EUR 2.00
    EXPECT_EQ(run.status(ReportType::BankDepositInterest), ReportStatus::NeedsInput);
}

TEST(IncomeReportsIntegrationTest, YesToTheAllowanceQuestionMakesTheDohDhoRow) {
    const TemporaryCsv csv{
        R"(2023-12-29T10:00:00Z,2023-12-29,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,2.00,,-0.50,EUR,,,,Synthetic bank interest,synthetic-interest-2,,,,)",
    };

    IncomeRun run{csv, 2023};

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_TRUE(run.answerFirst(DepositInterestTotalAnswer{.mAboveLimit = true}).empty());

    const auto& rows = run.reports().mDeposit.mRows;

    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].mInterest, 20'000);  // EUR 2.00
    EXPECT_EQ(rows[0].mForeignTax, 5'000); // EUR 0.50
    EXPECT_TRUE(run.result().mRequests.empty());
}

TEST(IncomeReportsIntegrationTest, NoToTheAllowanceQuestionGivesNoDohDhoWithItsReason) {
    const TemporaryCsv csv{
        R"(2023-12-29T10:00:00Z,2023-12-29,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,2.00,,-0.50,EUR,,,,Synthetic bank interest,synthetic-interest-2,,,,)",
    };

    IncomeRun run{csv, 2023};

    ASSERT_TRUE(run.answerFirst(DepositInterestTotalAnswer{.mAboveLimit = false}).empty());

    EXPECT_TRUE(run.reports().mDeposit.mRows.empty());
    EXPECT_EQ(run.status(ReportType::BankDepositInterest), ReportStatus::NoData);
    EXPECT_EQ(run.result().report(ReportType::BankDepositInterest).mNoDataReason,
              NoDataReason::BelowDepositInterestThreshold);
}

TEST(IncomeReportsIntegrationTest, AFailedCouponIsinCanBeLeftOutOfDohObr) {
    // Two EUR-amount coupons in 2024. The first has a USD original amount and no rate file is
    // loaded, so its rate is missing; the second is in EUR only.
    const TemporaryCsv csv{
        R"(2024-08-01T10:00:00Z,2024-08-01,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond A,XX0000000003,,,1.80,,,EUR,2.00,USD,0.900000,Synthetic bond interest,synthetic-coupon-1,,,,)",
        R"(2024-09-01T10:00:00Z,2024-09-01,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond B,XX0000000004,,,1.80,,,EUR,,,,Synthetic bond interest,synthetic-coupon-2,,,,)",
    };

    IncomeRun run{csv, 2024};

    // Open: the question lists only XX0000000003, and Doh-Obr needs the answer.
    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto* question = std::get_if<FailureResolutionQuestion>(&run.result().mRequests[0].mData);

    ASSERT_NE(question, nullptr);
    EXPECT_EQ(run.result().mRequests[0].mType, RequestType::OtherInterestFailureResolutionRequired);
    ASSERT_EQ(run.result().mRequests[0].mInvestments.size(), 1U);
    EXPECT_EQ(run.result().mRequests[0].mInvestments[0].mIsin, "XX0000000003");
    EXPECT_EQ(run.status(ReportType::OtherInterest), ReportStatus::NeedsInput);

    ASSERT_TRUE(run.answerFirst(kExclude).empty());

    // Excluded: XX0000000004 stays, and the form no longer fails.
    ASSERT_EQ(run.reports().mOther.mRows.size(), 1U);
    EXPECT_EQ(std::get<BondCoupon>(run.reports().mOther.mRows[0].mKind).mIsin, "XX0000000004");
    EXPECT_EQ(run.reports().mOther.mRows[0].mGrossInterest, 18'000); // EUR 1.80
    EXPECT_NE(run.status(ReportType::OtherInterest), ReportStatus::Failed);
    EXPECT_NE(run.status(ReportType::OtherInterest), ReportStatus::NeedsInput);
}

TEST(IncomeReportsIntegrationTest, FailingDohObrKeepsTheFailureOfThatFormOnly) {
    const TemporaryCsv csv{
        R"(2024-08-01T10:00:00Z,2024-08-01,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond A,XX0000000003,,,1.80,,,EUR,2.00,USD,0.900000,Synthetic bond interest,synthetic-coupon-1,,,,)",
        R"(2024-04-01T09:00:00Z,2024-04-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000002,,,10,,,EUR,,,,,synthetic-dividend-1,,,,)",
    };

    IncomeRun run{csv, 2024};

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_TRUE(run.answerFirst(kFailReport).empty());

    EXPECT_EQ(run.status(ReportType::OtherInterest), ReportStatus::Failed);
    EXPECT_NE(run.status(ReportType::Dividends), ReportStatus::Failed);
    EXPECT_EQ(run.reports().mDividends.mRows.size(), 1U);
}

TEST(IncomeReportsIntegrationTest, ADividendFailureLeavesTheOtherIsinAndTheOtherForms) {
    // XX0000000001 is paid in USD and no rate file is loaded; XX0000000002 is EUR 10.00.
    const TemporaryCsv csv{
        R"(2024-04-01T09:00:00Z,2024-04-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share A,XX0000000001,,,9,,,EUR,10,USD,0.900000,,synthetic-dividend-1,,,,)",
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share B,XX0000000002,,,10,,,EUR,,,,,synthetic-dividend-2,,,,)",
    };

    IncomeRun run{csv, 2024};

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    EXPECT_EQ(run.result().mRequests[0].mType, RequestType::DividendsFailureResolutionRequired);
    ASSERT_EQ(run.result().mRequests[0].mInvestments.size(), 1U);
    EXPECT_EQ(run.result().mRequests[0].mInvestments[0].mIsin, "XX0000000001");
    EXPECT_EQ(run.status(ReportType::Dividends), ReportStatus::NeedsInput);

    ASSERT_TRUE(run.answerFirst(kExclude).empty());

    ASSERT_EQ(run.reports().mDividends.mRows.size(), 1U);
    EXPECT_EQ(run.reports().mDividends.mRows[0].mIsin, "XX0000000002");
    EXPECT_EQ(run.reports().mDividends.mRows[0].mGrossIncome, 100'000); // EUR 10.00
    EXPECT_NE(run.status(ReportType::Dividends), ReportStatus::Failed);
    EXPECT_EQ(run.status(ReportType::CapitalGains), ReportStatus::NoData);
}

TEST(IncomeReportsIntegrationTest, AnUnknownInterestRowAsksItsTreatment) {
    // An interest payment of an instrument with the class STOCK has an unclear type.
    const TemporaryCsv csv{
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,CASH,INTEREST_PAYMENT,STOCK,Synthetic Payer,XX0000000001,,,12,,,EUR,,,,Synthetic interest,synthetic-interest-1,,,,)",
    };

    IncomeRun run{csv, 2024};

    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto* question =
        std::get_if<IncomeClassificationQuestion>(&run.result().mRequests[0].mData);

    ASSERT_NE(question, nullptr);
    EXPECT_EQ(question->mGrossAmount, 120'000); // EUR 12.00
    EXPECT_TRUE(run.reports().mOther.mRows.empty());

    ASSERT_TRUE(
        run.answerFirst(IncomeTreatmentAnswer{IncomeTreatment::BrokerCashInterest}).empty());

    ASSERT_EQ(run.reports().mOther.mRows.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<BrokerCashInterest>(run.reports().mOther.mRows[0].mKind));
    EXPECT_EQ(run.reports().mOther.mRows[0].mGrossInterest, 120'000);
}

TEST(IncomeReportsIntegrationTest, ACancelledIncomeTypeBlocksTheFormsItMayBelongTo) {
    // The same payment as above: with an ISIN it may be a dividend, a bank deposit interest or
    // a broker cash interest, so a cancel makes an error in all three forms.
    const TemporaryCsv csv{
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,CASH,INTEREST_PAYMENT,STOCK,Synthetic Payer,XX0000000001,,,12,,,EUR,,,,Synthetic interest,synthetic-interest-1,,,,)",
    };

    IncomeRun run{csv, 2024};

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_TRUE(run.answerFirst(CancelAnswer{}).empty());

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.reports().mOther.mRows.empty());
    EXPECT_TRUE(run.reports().mDeposit.mRows.empty());
    EXPECT_TRUE(run.reports().mDividends.mRows.empty());
    EXPECT_EQ(run.status(ReportType::Dividends), ReportStatus::Failed);
    EXPECT_EQ(run.status(ReportType::BankDepositInterest), ReportStatus::Failed);
    EXPECT_EQ(run.status(ReportType::OtherInterest), ReportStatus::Failed);
}

TEST(IncomeReportsIntegrationTest, AnExcludedIncomeTypeLeavesNoRowAndNoErrorButAnExcludedItem) {
    const TemporaryCsv csv{
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,CASH,INTEREST_PAYMENT,STOCK,Synthetic Payer,XX0000000001,,,12,,,EUR,,,,Synthetic interest,synthetic-interest-1,,,,)",
    };

    IncomeRun run{csv, 2024};

    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_TRUE(run.answerFirst(ExcludeFromReportsAnswer{}).empty());

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_TRUE(run.reports().mOther.mRows.empty());
    EXPECT_TRUE(run.reports().mDeposit.mRows.empty());
    EXPECT_TRUE(run.reports().mDividends.mRows.empty());
    EXPECT_NE(run.status(ReportType::OtherInterest), ReportStatus::Failed);
    EXPECT_EQ(run.diagnosticsOf(ProcessingDiagnosticCode::TaxCategoryNotReported).size(), 1U);

    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);
    EXPECT_EQ(run.result().mExcludedItems[0].mIsin, "XX0000000001");
}

TEST(IncomeReportsIntegrationTest, ASoldBondAsksItsTypeBeforeItsCouponsAreRows) {
    // A bond bought in 2025 (1000 units), two EUR coupons, then its maturity on 2025-08-14.
    const TemporaryCsv csv{
        R"(2025-02-03T09:00:00Z,2025-02-03,DEFAULT,TRADING,BUY,BOND,Synthetic Bond,XX0000000003,1000,1,-1000,,,EUR,,,,,synthetic-bond-buy-1,,,,)",
        R"(2025-02-14T16:00:00Z,2025-02-14,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond,XX0000000003,,,18,,,EUR,,,,Synthetic bond interest,synthetic-coupon-1,,,,)",
        R"(2025-08-14T15:00:00Z,2025-08-14,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond,XX0000000003,,,18.40,,,EUR,,,,Synthetic bond interest,synthetic-coupon-2,,,,)",
        R"(2025-08-14T15:01:00Z,2025-08-14,DEFAULT,CORPORATE_ACTION,FINAL_MATURITY,BOND,Synthetic Bond,XX0000000003,-1000,,,,,,,,,FINAL_MATURITY XX0000000003,synthetic-bond-maturity-1,,,,)",
    };

    // Open: Doh-Obr needs the type and has no coupon row.
    IncomeRun open{csv, 2025};

    ASSERT_EQ(open.result().mRequests.size(), 1U);
    EXPECT_EQ(open.result().mRequests[0].mType, RequestType::IncomeDetailsRequired);
    EXPECT_EQ(open.status(ReportType::OtherInterest), ReportStatus::NeedsInput);
    EXPECT_TRUE(open.reports().mOther.mRows.empty());

    // Ordinary: both coupons are rows.
    IncomeRun ordinary{csv, 2025};

    ASSERT_TRUE(ordinary.answerFirst(BondTypeAnswer{BondType::Ordinary}).empty());
    ASSERT_EQ(ordinary.reports().mOther.mRows.size(), 2U);
    EXPECT_EQ(ordinary.reports().mOther.mRows[0].mGrossInterest, 180'000); // EUR 18.00
    EXPECT_EQ(ordinary.reports().mOther.mRows[1].mGrossInterest, 184'000); // EUR 18.40
    EXPECT_NE(ordinary.status(ReportType::OtherInterest), ReportStatus::NeedsInput);

    // Discount or zero-coupon: its interest on the maturity is not calculated, so the bond has an
    // error and Doh-Obr asks to leave it out or fail. Failing it fails only Doh-Obr.
    IncomeRun discount{csv, 2025};

    ASSERT_TRUE(discount.answerFirst(BondTypeAnswer{BondType::DiscountOrZeroCoupon}).empty());
    ASSERT_EQ(discount.result().mRequests.size(), 1U);
    EXPECT_EQ(discount.result().mRequests[0].mType,
              RequestType::OtherInterestFailureResolutionRequired);
    ASSERT_EQ(discount.result().mRequests[0].mInvestments.size(), 1U);
    EXPECT_EQ(discount.result().mRequests[0].mInvestments[0].mIsin, "XX0000000003");
    EXPECT_EQ(
        discount.result().mRequests[0].mInvestments[0].mReasonCodes,
        std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::UnsupportedTaxTreatment});
    EXPECT_TRUE(discount.reports().mOther.mRows.empty());

    ASSERT_TRUE(discount.answerFirst(kFailReport).empty());
    EXPECT_EQ(discount.status(ReportType::OtherInterest), ReportStatus::Failed);
}

} // namespace
