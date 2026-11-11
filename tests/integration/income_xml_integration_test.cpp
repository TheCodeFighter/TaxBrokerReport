#include "../support/xsd_validation.hpp"
#include "parsers/traderepublic_parser.hpp"
#include "processors/income_reports.hpp"
#include "processors/tax_processor.hpp"
#include "taxbroker/currency.hpp"
#include "taxbroker/decision_session.hpp"
#include "taxbroker/official_rates.hpp"
#include "taxbroker/reference_catalogue.hpp"
#include "utils/date_utils.hpp"
#include "xml/dho_xml_writer.hpp"
#include "xml/div_xml_writer.hpp"
#include "xml/obr_xml_writer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
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
using taxbroker::test::expectValidForDhoSchema;
using taxbroker::test::expectValidForDivSchema;
using taxbroker::test::expectValidForObrSchema;

// All dates and amounts here are synthetic; the named companies take their payer facts from the
// built-in reference files. Money is stored with 4 decimals, so 100'000 is EUR 10.00. The ISINs
// with the prefix XX are never in the built-in payer file, so no payer is found. Each test adds
// its own official rates with addRate.

/// Writes the synthetic Trade Republic rows of one scenario to a CSV file and removes it again.
class TemporaryCsv {
  public:
    explicit TemporaryCsv(std::initializer_list<std::string_view> aRows) {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();

        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_income_xml_synthetic_" + std::to_string(token));

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
    IncomeRun(const std::filesystem::path& aCsv, int aYear, OfficialRateTable aRates)
        : IncomeRun(aCsv, aYear, std::move(aRates), referenceCatalogue()) {}

    /// The run with this reference data instead of the built-in files.
    IncomeRun(const std::filesystem::path& aCsv,
              int aYear,
              OfficialRateTable aRates,
              ReferenceCatalogue aCatalogue)
        : mRates(std::move(aRates)), mCatalogue(std::move(aCatalogue)) {
        tr::TradeRepublicParser parser;
        std::vector<StatementMergeInput> inputs{{0, parser.parse(aCsv)}};

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
                                            mCatalogue,
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
    ReferenceCatalogue mCatalogue;
    DecisionSession mSession;
    IncomeReports mReports;
};

/// The full synthetic taxpayer of `tests/test_data/cli/taxpayer.json`, as the report command
/// reads it.
ReportMetadata metadata(int aYear, FilingType aFilingType) {
    return ReportMetadata{
        .mTaxpayer = {.mTaxNumber = "12345678",
                      .mName = "Synthetic Taxpayer",
                      .mAddress = "Synthetic Street 1",
                      .mCity = "Synthetic City",
                      .mPostCode = "1000",
                      .mPostName = "Synthetic City",
                      .mBirthDate = *parseCalendarDate("1990-01-31"),
                      .mIsResident = true,
                      .mEmail = "taxpayer@example.com",
                      .mPhoneNumber = "+38600000000"},
        .mReportingYear = aYear,
        .mFilingType = aFilingType,
    };
}

/// A committed file under `tests/test_data`, read byte for byte.
std::string readTestData(std::string_view aFolder, std::string_view aName) {
    const auto path =
        std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / aFolder / aName;
    std::ifstream input{path, std::ios::binary};

    if (!input.is_open())
    {
        throw std::runtime_error{"Unable to open the golden file " + std::string{aName}};
    }

    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/// Marks the rate file of 2025 as loaded, from 2 January to 31 December.
void addFile2025(OfficialRateTable& aTable) {
    aTable.mFiles.push_back(
        OfficialRateFile{.mSourceFile = SourceFilename::fromPath("exr_2025.csv"),
                         .mYear = 2025,
                         .mFirstDate = *parseCalendarDate("2025-01-02"),
                         .mLastDate = *parseCalendarDate("2025-12-31"),
                         .mRateCount = 2,
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
                     .mSourceFile = SourceFilename::fromPath("exr_2025.csv"),
                     .mSourceRow = aRow,
                     .mSource = OfficialRateSource::EcbReferenceRateFile,
                     .mOrigin = OfficialRateOrigin::Supplied};
}

TEST(IncomeXmlIntegrationTest, DohDivFromFiveDividendsMatchesTheGoldenFile) {
    // Rates (USD per 1 EUR): 1.25 on 2025-03-14, 1.28 on 2025-05-15.
    // R1 USD 12.53 / 1.25 = EUR 10.024 -> Value 10.02, no tax column -> ForeignTax 0.00.
    // R2 USD 20.00 / 1.28 = EUR 15.625 -> Value 15.63 (half up), tax EUR 2.40, the US treaty text.
    // R3 EUR 5.00, an ISIN with no reference row: no payer facts, warning.
    // R4 EUR 3.00 of Krka (Slovenian payer): no row.
    // R5 EUR 4.00 of a Brazilian payer with a non-ASCII address.
    const TemporaryCsv csv{
        R"(2025-03-14T09:00:00Z,2025-03-14,DEFAULT,CASH,DIVIDEND,STOCK,Apple Inc,US0378331005,,,10.02,,,EUR,12.53,USD,0.800000,,synthetic-dividend-1,,,,)",
        R"(2025-05-15T09:00:00Z,2025-05-15,DEFAULT,CASH,DIVIDEND,STOCK,Johnson & Johnson,US4781601046,,,15.63,,-2.40,EUR,20.00,USD,0.781250,,synthetic-dividend-2,,,,)",
        R"(2025-06-20T09:00:00Z,2025-06-20,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Fund,XX0000000010,,,5.00,,,EUR,,,,,synthetic-dividend-3,,,,)",
        R"(2025-07-01T09:00:00Z,2025-07-01,DEFAULT,CASH,DIVIDEND,STOCK,Krka d.d.,SI0031102120,,,3.00,,,EUR,,,,,synthetic-dividend-4,,,,)",
        R"(2025-09-10T09:00:00Z,2025-09-10,DEFAULT,CASH,DIVIDEND,STOCK,Banco Bradesco SA,US0594603039,,,4.00,,,EUR,,,,,synthetic-dividend-5,,,,)",
    };

    OfficialRateTable rates;

    addFile2025(rates);
    addRate(rates, "USD", "2025-03-14", 125'000'000, 2);
    addRate(rates, "USD", "2025-05-15", 128'000'000, 3);

    const IncomeRun run{csv.path(), 2025, std::move(rates)};

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_NE(run.status(ReportType::Dividends), ReportStatus::NeedsInput);
    EXPECT_NE(run.status(ReportType::Dividends), ReportStatus::Failed);

    const auto& rows = run.reports().mDividends.mRows;

    ASSERT_EQ(rows.size(), 4U);
    EXPECT_EQ(rows[0].mGrossIncome, 100'240);            // EUR 10.024 at 4 decimals
    EXPECT_EQ(rows[1].mGrossIncome, 156'250);            // EUR 15.625
    EXPECT_EQ(rows[1].mWithholding.mForeignTax, 24'000); // EUR 2.40
    EXPECT_EQ(rows[2].mGrossIncome, 50'000);             // EUR 5.00
    EXPECT_EQ(rows[3].mGrossIncome, 40'000);             // EUR 4.00

    // One warning for R3 only: the payer is not in the reference files.
    EXPECT_EQ(run.diagnosticsOf(ProcessingDiagnosticCode::DividendPayerNotFound).size(), 1U);

    // R4 is left out: Krka deducts the Slovenian tax itself.
    ASSERT_EQ(run.result().mExcludedItems.size(), 1U);

    const auto& excluded = run.result().mExcludedItems.at(0);

    EXPECT_EQ(excluded.mIsin, "SI0031102120");
    EXPECT_EQ(excluded.mCategory, ExcludedCategory::Dividend);
    EXPECT_EQ(excluded.mReason, ExcludedReason::SlovenianTaxSettled);

    const auto result =
        DivXmlWriter::write(metadata(2025, FilingType::Original), run.reports().mDividends);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, readTestData("income", "Doh-Div_2025.golden.xml"));
    expectValidForDivSchema(*result.mXml);
}

TEST(IncomeXmlIntegrationTest, DohDhoFromThreeInterestPaymentsIsOneRowMatchingTheGoldenFile) {
    // Three payments of Trade Republic bank interest: EUR 0.33 + 0.33 + 0.34 = EUR 1.00, one row.
    const TemporaryCsv csv{
        R"(2025-01-31T10:00:00Z,2025-01-31,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,0.33,,,EUR,,,,Synthetic bank interest,synthetic-interest-1,,,,)",
        R"(2025-02-28T10:00:00Z,2025-02-28,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,0.33,,,EUR,,,,Synthetic bank interest,synthetic-interest-2,,,,)",
        R"(2025-03-31T10:00:00Z,2025-03-31,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,0.34,,,EUR,,,,Synthetic bank interest,synthetic-interest-3,,,,)",
    };

    IncomeRun run{csv.path(), 2025, OfficialRateTable{}};

    // EUR 1.00 is at most EUR 1,000, so the allowance question comes first; yes makes the row.
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_TRUE(run.answerFirst(DepositInterestTotalAnswer{.mAboveLimit = true}).empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_NE(run.status(ReportType::BankDepositInterest), ReportStatus::NeedsInput);
    EXPECT_NE(run.status(ReportType::BankDepositInterest), ReportStatus::Failed);

    const auto& rows = run.reports().mDeposit.mRows;

    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].mInterest, 10'000); // EUR 1.00
    EXPECT_EQ(rows[0].mForeignTax, 0);

    const auto result =
        DhoXmlWriter::write(metadata(2025, FilingType::Original), run.reports().mDeposit);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, readTestData("income", "Doh-DHO_2025.golden.xml"));
    expectValidForDhoSchema(*result.mXml);
}

TEST(IncomeXmlIntegrationTest, DohObrFromTheBondFixtureCouponsMatchesTheGoldenFile) {
    // The committed bond fixture has two coupons of USD 20.00 (2025-02-14 and 2025-08-14). Rates
    // (USD per 1 EUR): 1.25 and 1.28. 20.00 / 1.25 = EUR 16.00; 20.00 / 1.28 = EUR 15.625 -> 15.63.
    // The accrued interest paid with the purchase does not lower the coupon.
    const auto fixture = std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" /
                         "csv" / "traderepublic_bond_maturity_fixture.csv";
    OfficialRateTable rates;

    addFile2025(rates);
    addRate(rates, "USD", "2025-02-14", 125'000'000, 2);
    addRate(rates, "USD", "2025-08-14", 128'000'000, 3);

    IncomeRun run{fixture, 2025, std::move(rates)};

    // The bond was sold at maturity, so its type is asked first.
    ASSERT_EQ(run.result().mRequests.size(), 1U);
    ASSERT_TRUE(run.answerFirst(BondTypeAnswer{BondType::Ordinary}).empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_NE(run.status(ReportType::OtherInterest), ReportStatus::NeedsInput);
    EXPECT_NE(run.status(ReportType::OtherInterest), ReportStatus::Failed);

    const auto& rows = run.reports().mOther.mRows;

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].mGrossInterest, 160'000); // EUR 16.00
    EXPECT_EQ(rows[1].mGrossInterest, 156'250); // EUR 15.625

    const auto result =
        ObrXmlWriter::write(metadata(2025, FilingType::Original), run.reports().mOther);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, readTestData("income", "Doh-Obr_2025.golden.xml"));
    expectValidForObrSchema(*result.mXml);
}

TEST(IncomeXmlIntegrationTest, DohObrSelfReportRunWithBothSubtypesIsWrittenAsAnOriginal) {
    // 2023-06-15: a coupon of EUR 25.00 of a bond bought and still held (no type question).
    // 2023-11-30: Trade Republic interest of EUR 3.45 before 6 December 2023, so cash interest.
    const TemporaryCsv csv{
        R"(2023-01-10T10:00:00Z,2023-01-10,DEFAULT,TRADING,BUY,BOND,Synthetic Bond,XX0000000020,1000,1,-1000,,,EUR,,,,,synthetic-bond-buy-1,,,,)",
        R"(2023-06-15T10:00:00Z,2023-06-15,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond,XX0000000020,,,25.00,,,EUR,,,,Synthetic bond interest,synthetic-coupon-1,,,,)",
        R"(2023-11-30T10:00:00Z,2023-11-30,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,3.45,,,EUR,,,,Synthetic broker interest,synthetic-interest-1,,,,)",
    };

    const IncomeRun run{csv.path(), 2023, OfficialRateTable{}};

    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_NE(run.status(ReportType::OtherInterest), ReportStatus::NeedsInput);
    EXPECT_NE(run.status(ReportType::OtherInterest), ReportStatus::Failed);

    const auto& rows = run.reports().mOther.mRows;

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<BondCoupon>(rows[0].mKind));
    EXPECT_EQ(rows[0].mGrossInterest, 250'000); // EUR 25.00
    EXPECT_TRUE(std::holds_alternative<BrokerCashInterest>(rows[1].mKind));
    EXPECT_EQ(rows[1].mGrossInterest, 34'500); // EUR 3.45

    const auto result =
        ObrXmlWriter::write(metadata(2023, FilingType::SelfReport), run.reports().mOther);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, readTestData("income", "Doh-Obr_2023.golden.xml"));
    expectValidForObrSchema(*result.mXml);
}

TEST(IncomeXmlIntegrationTest, DohObrWithPayersForeignTaxAndTreatyClaimsMatchesTheGoldenFile) {
    // 2023-03-14: a coupon of EUR 25.00 with EUR 3.00 foreign tax; the issuer is in US and the
    //             reference files have a bond coupon text for US: the row has that text.
    // 2023-06-16: a coupon of EUR 40.00 with EUR 6.00 foreign tax; the issuer is in DE and there
    //             is no bond coupon text for DE, so the question is answered yes: `DA`.
    // 2023-11-30: Trade Republic interest of EUR 3.45 before 6 December 2023, so cash interest,
    //             without tax: the broker's payer facts and no ReliefStatement.
    // Both bonds are bought on 2023-01-10 and kept, so there is no bond type question.
    const TemporaryCsv csv{
        R"(2023-01-10T10:00:00Z,2023-01-10,DEFAULT,TRADING,BUY,BOND,Synthetic Bond US,XX0000000010,1000,1,-1000,,,EUR,,,,,synthetic-bond-buy-1,,,,)",
        R"(2023-01-10T10:01:00Z,2023-01-10,DEFAULT,TRADING,BUY,BOND,Synthetic Bond DE,XX0000000028,1000,1,-1000,,,EUR,,,,,synthetic-bond-buy-2,,,,)",
        R"(2023-03-14T10:00:00Z,2023-03-14,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond US,XX0000000010,,,25.00,,-3.00,EUR,,,,Synthetic bond interest,synthetic-coupon-1,,,,)",
        R"(2023-06-16T10:00:00Z,2023-06-16,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond DE,XX0000000028,,,40.00,,-6.00,EUR,,,,Synthetic bond interest,synthetic-coupon-2,,,,)",
        R"(2023-11-30T10:00:00Z,2023-11-30,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,3.45,,,EUR,,,,Synthetic broker interest,synthetic-interest-1,,,,)",
    };
    const auto catalogue = parseReferenceCatalogue(
        "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
        "source_revision\n"
        "XX0000000010,Synthetic Issuer US,TAX-U,1 Main Street,US,bond,,issuer,manual,\n"
        "XX0000000028,Synthetic Issuer DE,TAX-D,2 Main Street,DE,bond,,issuer,manual,\n",
        "country,income,statement,source,source_revision\n"
        "US,bond_coupon,2. odstavek 11. člena,manual,\n",
        "broker,code,name,tax_number,address,country,source,source_revision\n"
        "TR,TR-X,Synthetic Broker,TAX-T,\"3 Main Street, Berlin\",DE,manual,\n");

    IncomeRun run{csv.path(), 2023, OfficialRateTable{}, catalogue};

    // Only the DE coupon has no text, so only DE is asked; the US coupon is claimed with its text.
    ASSERT_EQ(run.result().mRequests.size(), 1U);

    const auto* question =
        std::get_if<InterestTreatyClaimQuestion>(&run.result().mRequests.at(0).mData);

    ASSERT_NE(question, nullptr);
    EXPECT_EQ(question->mCountry, "DE");
    EXPECT_EQ(question->mIsins, std::vector<Isin>{"XX0000000028"});
    EXPECT_FALSE(question->mCashInterest);

    ASSERT_TRUE(run.answerFirst(InterestTreatyClaimAnswer{.mClaim = true}).empty());
    EXPECT_TRUE(run.result().mRequests.empty());
    EXPECT_EQ(run.diagnosticsOf(ProcessingDiagnosticCode::TreatyClaimDetailsMissing).size(), 1U);
    EXPECT_NE(run.status(ReportType::OtherInterest), ReportStatus::NeedsInput);
    EXPECT_NE(run.status(ReportType::OtherInterest), ReportStatus::Failed);

    const auto& rows = run.reports().mOther.mRows;

    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].mGrossInterest, 250'000); // EUR 25.00
    EXPECT_EQ(rows[1].mGrossInterest, 400'000); // EUR 40.00
    EXPECT_EQ(rows[2].mGrossInterest, 34'500);  // EUR 3.45

    // US: the reference text; DE: claimed without a text (`DA`); the cash interest: no claim.
    ASSERT_TRUE(rows[0].mTreatyClaim.has_value());
    EXPECT_EQ(rows[0].mTreatyClaim->mStatement,
              std::optional<std::string>{"2. odstavek 11. člena"});
    ASSERT_TRUE(rows[1].mTreatyClaim.has_value());
    EXPECT_EQ(rows[1].mTreatyClaim->mStatement, std::nullopt);
    EXPECT_FALSE(rows[2].mTreatyClaim.has_value());

    const auto result =
        ObrXmlWriter::write(metadata(2023, FilingType::Original), run.reports().mOther);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, readTestData("income", "Doh-Obr_2023_treaty.golden.xml"));
    expectValidForObrSchema(*result.mXml);
}

} // namespace
