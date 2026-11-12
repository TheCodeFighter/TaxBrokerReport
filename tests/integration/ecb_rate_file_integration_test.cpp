#include "processors/official_rate_selection.hpp"
#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/official_rates.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace taxbroker;

std::filesystem::path fixturePath(std::string_view aFilename) {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / "ecb" /
           aFilename;
}

Date date(std::string_view aText) {
    return *parseCalendarDate(aText);
}

TEST(EcbRateFileIntegrationTest, LoadsTheSyntheticFixtureAndConvertsAtTheExactDate) {
    const auto result = loadEcbRateFiles({fixturePath("exr_2025_synthetic.csv")});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mTable.mRates.size(), 4U);
    ASSERT_EQ(result.mTable.mFiles.size(), 1U);

    EXPECT_EQ(result.mTable.mFiles[0].mSourceFile.value(), "exr_2025_synthetic.csv");
    EXPECT_EQ(result.mTable.mFiles[0].mYear, 2025);
    EXPECT_EQ(result.mTable.mFiles[0].mRateCount, 4U);

    const OfficialRate* dollar = result.mTable.find(Currency::USD, date("2025-01-03"));

    ASSERT_NE(dollar, nullptr);
    EXPECT_EQ(dollar->mValue, 126'250'000); // 1.2625
    EXPECT_EQ(dollar->mSeriesKey, "EXR.D.USD.EUR.SP00.A");
    EXPECT_EQ(dollar->mSourceRow, 5U); // header, GBP, GBP, USD, USD

    // 12.6250 USD / 1.2625 = 10.0000 EUR
    const auto converted =
        convertMoney(126'250, dollar->mValue, ExchangeRateDirection::ForeignPerEur);

    ASSERT_TRUE(std::holds_alternative<Money>(converted));
    EXPECT_EQ(std::get<Money>(converted), 100'000);

    // 8.0000 GBP / 0.8 = 10.0000 EUR
    const OfficialRate* pound = result.mTable.find(Currency::GBP, date("2025-01-02"));

    ASSERT_NE(pound, nullptr);

    const auto convertedPound =
        convertMoney(80'000, pound->mValue, ExchangeRateDirection::ForeignPerEur);

    ASSERT_TRUE(std::holds_alternative<Money>(convertedPound));
    EXPECT_EQ(std::get<Money>(convertedPound), 100'000);
}

TEST(EcbRateFileIntegrationTest, HasNoRateForADayWithoutARow) {
    const auto result = loadEcbRateFiles({fixturePath("exr_2025_synthetic.csv")});

    ASSERT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.mTable.find(Currency::USD, date("2025-01-04")), nullptr); // a Saturday
    EXPECT_EQ(result.mTable.find(Currency::USD, date("2025-01-01")), nullptr); // before the file
    EXPECT_EQ(result.mTable.find(Currency::CHF, date("2025-01-02")), nullptr); // not in the file
}

SourceReference sourceRow(std::size_t aRow) {
    return SourceReference{
        .mBroker = Broker::TradeRepublic,
        .mFilename = SourceFilename::fromPath("history.csv"),
        .mSourceRow = aRow,
        .mTransactionId = "a",
    };
}

TEST(EcbRateFileIntegrationTest, SelectsAndConvertsAtTheExactDateOfALoadedFile) {
    const auto result = loadEcbRateFiles({fixturePath("exr_2025_synthetic.csv")});
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-01-03"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {sourceRow(2)}},
    };

    ASSERT_TRUE(result.mDiagnostics.empty());

    const auto selection = selectOfficialRates(needs, result.mTable);

    EXPECT_TRUE(selection.mDiagnostics.empty());

    const OfficialRate* rate = selection.find(Currency::USD, date("2025-01-03"));

    ASSERT_NE(rate, nullptr);
    EXPECT_EQ(rate->mValue, 126'250'000); // 1.2625
    EXPECT_EQ(rate->mSourceRow, 5U);      // header, GBP, GBP, USD, USD

    // 12.6250 USD / 1.2625 = 10.0000 EUR
    const auto converted = convertOfficialMoney(126'250, *rate);

    ASSERT_TRUE(std::holds_alternative<Money>(converted));
    EXPECT_EQ(std::get<Money>(converted), 100'000);
}

TEST(EcbRateFileIntegrationTest, TakesTheLastRateOfThePreviousYearOnNewYearsDay) {
    const auto result = loadEcbRateFiles(
        {fixturePath("exr_2024_synthetic.csv"), fixturePath("exr_2025_synthetic.csv")});
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-01-01"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {sourceRow(2)}},
    };

    ASSERT_TRUE(result.mDiagnostics.empty());

    const auto selection = selectOfficialRates(needs, result.mTable);

    // 2025-01-01 is a TARGET holiday; the 2024 file ends on 2024-12-31 (1.0389).
    const OfficialRate* rate = selection.find(Currency::USD, date("2025-01-01"));

    ASSERT_NE(rate, nullptr);
    EXPECT_EQ(rate->mValue, 103'890'000);
    EXPECT_EQ(rate->mObservationDate, date("2024-12-31"));
    EXPECT_EQ(rate->mSourceFile.value(), "exr_2024_synthetic.csv");

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey, MessageKey::OfficialRateFromNearbyDate);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "USD"),
                                         textParam("date", "2025-01-01"),
                                         textParam("rateDate", "2024-12-31")}));
}

TEST(EcbRateFileIntegrationTest, BlocksNewYearsDayWhenThePreviousYearsFileIsNotLoaded) {
    const auto result = loadEcbRateFiles({fixturePath("exr_2025_synthetic.csv")});
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-01-01"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {sourceRow(2)}},
    };

    ASSERT_TRUE(result.mDiagnostics.empty());

    const auto selection = selectOfficialRates(needs, result.mTable);

    EXPECT_EQ(selection.find(Currency::USD, date("2025-01-01")), nullptr);

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey, MessageKey::MissingExchangeRateNoRateFile);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "USD"),
                                         textParam("date", "2025-01-01"),
                                         numberParam("year", 2024)}));
}

TEST(EcbRateFileIntegrationTest, BlocksACurrencyThatIsNotInTheLoadedFile) {
    const auto result = loadEcbRateFiles({fixturePath("exr_2025_synthetic.csv")});
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::CHF,
         .mTaxDate = date("2025-01-02"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {sourceRow(2)}},
    };

    ASSERT_TRUE(result.mDiagnostics.empty());

    const auto selection = selectOfficialRates(needs, result.mTable);

    // 2025-01-02 has GBP and USD rates, so it is an ECB day; the file has no CHF at all.
    EXPECT_EQ(selection.find(Currency::CHF, date("2025-01-02")), nullptr);

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateNoRateOnDate);
    EXPECT_EQ(
        selection.mDiagnostics.at(0).mMessageParams,
        (std::vector<MessageParam>{textParam("currency", "CHF"), textParam("date", "2025-01-02")}));
}

TEST(EcbRateFileIntegrationTest, BlocksADateAfterTheLastDayOfAFileThatEndsEarly) {
    const auto result = loadEcbRateFiles({fixturePath("exr_2025_synthetic.csv")});
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-01-06"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {sourceRow(2)}},
    };

    ASSERT_TRUE(result.mDiagnostics.empty());

    const auto selection = selectOfficialRates(needs, result.mTable);

    // The fixture ends on 2025-01-03, far from 31 December.
    EXPECT_EQ(selection.find(Currency::USD, date("2025-01-06")), nullptr);

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateRateFileEndsEarly);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "USD"),
                                         textParam("date", "2025-01-06"),
                                         textParam("lastDate", "2025-01-03")}));
}

TEST(EcbRateFileIntegrationTest, WarnsThatALoadedFileStartsLateInItsYear) {
    const auto result = loadEcbRateFiles({fixturePath("exr_2024_synthetic.csv")});
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2024-12-30"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {sourceRow(2)}},
    };

    ASSERT_TRUE(result.mDiagnostics.empty());

    const auto selection = selectOfficialRates(needs, result.mTable);

    // The 2024 fixture has only 2024-12-30 and 2024-12-31, so it starts almost at the year's end.
    ASSERT_NE(selection.find(Currency::USD, date("2024-12-30")), nullptr);

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::OfficialRateFileIncompleteStartsLate);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{numberParam("year", 2024),
                                         textParam("firstDate", "2024-12-30")}));
}

// The tests below load a broken rate file from disk next to the good 2024 fixture, so a rejected
// file must not change what the good file gives. The files have only the nine columns the loader
// needs; the loader ignores the other columns of the ECB export.
constexpr std::string_view kShortHeader =
    "KEY,FREQ,CURRENCY,CURRENCY_DENOM,EXR_TYPE,EXR_SUFFIX,TIME_PERIOD,OBS_VALUE,OBS_STATUS";

/// A path under the system temp directory that no other test of this run uses.
std::filesystem::path uniqueDirectoryPath() {
    static std::atomic<unsigned> counter{0};

    return std::filesystem::temp_directory_path() /
           ("taxbroker_ecb_integration_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(counter++));
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() : mPath{uniqueDirectoryPath()} {
        std::filesystem::create_directories(mPath);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;

        std::filesystem::remove_all(mPath, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    std::filesystem::path write(std::string_view aName, std::string_view aContent) const {
        const auto filePath = mPath / aName;
        std::ofstream output{filePath, std::ios::binary | std::ios::trunc};

        output << aContent;

        if (!output)
        {
            throw std::runtime_error{"Unable to write synthetic rate file"};
        }

        return filePath;
    }

  private:
    std::filesystem::path mPath;
};

RateNeed usdSaleNeed(std::string_view aTaxDate) {
    return RateNeed{.mCurrency = Currency::USD,
                    .mTaxDate = date(aTaxDate),
                    .mUse = RateUse::SalePrice,
                    .mReport = ReportType::CapitalGains,
                    .mSources = {sourceRow(2)}};
}

/// The 2025 rate is blocked because no 2025 file was loaded.
void expectBlockedYear2025(const OfficialRateTable& aTable) {
    const auto selection = selectOfficialRates({usdSaleNeed("2025-01-02")}, aTable);

    EXPECT_EQ(selection.find(Currency::USD, date("2025-01-02")), nullptr);

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey, MessageKey::MissingExchangeRateNoRateFile);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "USD"),
                                         textParam("date", "2025-01-02"),
                                         numberParam("year", 2025)}));
}

/// The good 2024 file still converts: 10.3890 USD / 1.0389 = 10.0000 EUR.
void expectGood2024FileStillConverts(const OfficialRateTable& aTable) {
    const auto selection = selectOfficialRates({usdSaleNeed("2024-12-31")}, aTable);

    EXPECT_TRUE(selection.mDiagnostics.empty() ||
                selection.mDiagnostics.at(0).mSeverity == DiagnosticSeverity::Warning);

    const OfficialRate* rate = selection.find(Currency::USD, date("2024-12-31"));

    ASSERT_NE(rate, nullptr);
    EXPECT_EQ(rate->mValue, 103'890'000); // 1.0389

    const auto converted = convertOfficialMoney(103'890, *rate);

    ASSERT_TRUE(std::holds_alternative<Money>(converted));
    EXPECT_EQ(std::get<Money>(converted), 100'000);
}

TEST(EcbRateFileIntegrationTest, RejectsAFileWithAnUnclosedQuoteAndKeepsTheGoodFile) {
    TemporaryDirectory directory;

    // The quote before "x is never closed.
    const auto badPath = directory.write(
        "exr_2025_bad.csv",
        std::string{kShortHeader} + "\n" +
            R"csv("EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A)csv" + "\n");
    const auto result = loadEcbRateFiles({fixturePath("exr_2024_synthetic.csv"), badPath});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(result.mDiagnostics.at(0).mSourceFile, "exr_2025_bad.csv");
    EXPECT_EQ(result.mDiagnostics.at(0).mRowIndex, std::nullopt);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::ParseErrorUnclosedQuote);
    ASSERT_EQ(result.mTable.mFiles.size(), 1U);
    EXPECT_EQ(result.mTable.mFiles.at(0).mSourceFile.value(), "exr_2024_synthetic.csv");

    expectGood2024FileStillConverts(result.mTable);
    expectBlockedYear2025(result.mTable);
}

TEST(EcbRateFileIntegrationTest, RejectsTwoRowsMergedByAQuoteThatClosesOnTheNextLine) {
    TemporaryDirectory directory;

    // The quotes are even in number, but the one in row 2 closes only in row 3. The reader then
    // merges rows 2 and 3 into one record.
    const auto badPath = directory.write(
        "exr_2025_bad.csv",
        std::string{kShortHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,"A)csv" + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-03,1.26,A")csv" + "\n");
    const auto result = loadEcbRateFiles({fixturePath("exr_2024_synthetic.csv"), badPath});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(result.mDiagnostics.at(0).mSourceFile, "exr_2025_bad.csv");
    EXPECT_EQ(result.mDiagnostics.at(0).mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::ParseErrorUnclosedQuote);

    expectGood2024FileStillConverts(result.mTable);
    expectBlockedYear2025(result.mTable);
}

TEST(EcbRateFileIntegrationTest, RejectsAHeaderWithARequiredColumnTwice) {
    TemporaryDirectory directory;

    // OBS_VALUE is named twice, so it is unclear which column holds the rate.
    const auto badPath = directory.write(
        "exr_2025_bad.csv",
        std::string{kShortHeader} + ",OBS_VALUE\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,9.99)csv" + "\n");
    const auto result = loadEcbRateFiles({fixturePath("exr_2024_synthetic.csv"), badPath});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.at(0).mSourceFile, "exr_2025_bad.csv");
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey,
              MessageKey::InvalidHeaderRateFileDuplicateColumn);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("column", "OBS_VALUE")}));

    expectGood2024FileStillConverts(result.mTable);
    expectBlockedYear2025(result.mTable);
}

TEST(EcbRateFileIntegrationTest, RejectsTheWholeFileWhenALaterRowHasTheWrongFieldCount) {
    TemporaryDirectory directory;

    // Rows 2 and 3 are good. Row 4 has 8 fields instead of 9, so none of the rows is loaded.
    const auto badPath = directory.write(
        "exr_2025_bad.csv",
        std::string{kShortHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A)csv" + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-03,1.26,A)csv" + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-06,1.27)csv" + "\n");
    const auto result = loadEcbRateFiles({fixturePath("exr_2024_synthetic.csv"), badPath});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, DiagnosticCode::FieldCountMismatch);
    EXPECT_EQ(result.mDiagnostics.at(0).mSourceFile, "exr_2025_bad.csv");
    EXPECT_EQ(result.mDiagnostics.at(0).mRowIndex, std::optional<std::size_t>{4});
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::FieldCountMismatchRateFile);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{numberParam("expected", 9U), numberParam("found", 8U)}));
    EXPECT_EQ(result.mTable.mFiles.size(), 1U);
    EXPECT_EQ(result.mTable.find(Currency::USD, date("2025-01-03")), nullptr);

    expectGood2024FileStillConverts(result.mTable);
    expectBlockedYear2025(result.mTable);
}

TEST(EcbRateFileIntegrationTest, RejectsAFileWithAnEstimatedRateAndBlocksItsYear) {
    TemporaryDirectory directory;

    // Row 3 has the status E (estimated); only A is accepted.
    const auto badPath = directory.write(
        "exr_2025_bad.csv",
        std::string{kShortHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A)csv" + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-03,1.26,E)csv" + "\n");
    const auto result = loadEcbRateFiles({fixturePath("exr_2024_synthetic.csv"), badPath});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.at(0).mSourceFile, "exr_2025_bad.csv");
    EXPECT_EQ(result.mDiagnostics.at(0).mRowIndex, std::optional<std::size_t>{3});
    EXPECT_EQ(result.mDiagnostics.at(0).mField, std::optional<std::string>{"OBS_STATUS"});
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::InvalidValueRateFieldExpected);
    EXPECT_EQ(
        result.mDiagnostics.at(0).mMessageParams,
        (std::vector<MessageParam>{textParam("field", "OBS_STATUS"), textParam("expected", "A")}));

    expectGood2024FileStillConverts(result.mTable);
    expectBlockedYear2025(result.mTable);
}

TEST(EcbRateFileIntegrationTest, RejectsTwoFilesOfTheSameYearAndKeepsAnotherYear) {
    TemporaryDirectory directory;

    // Both files cover 2025. Each alone would be valid, but it is unclear which one is right.
    const auto firstPath = directory.write(
        "first_2025.csv",
        std::string{kShortHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A)csv" + "\n");
    const auto secondPath = directory.write(
        "second_2025.csv",
        std::string{kShortHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.26,A)csv" + "\n");
    const auto result =
        loadEcbRateFiles({fixturePath("exr_2024_synthetic.csv"), firstPath, secondPath});

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    EXPECT_EQ(result.mDiagnostics.at(0).mCode, DiagnosticCode::DuplicateRateYear);
    EXPECT_EQ(result.mDiagnostics.at(0).mSourceFile, "first_2025.csv");
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::DuplicateRateYear);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{numberParam("year", 2025U)}));

    EXPECT_EQ(result.mDiagnostics.at(1).mCode, DiagnosticCode::DuplicateRateYear);
    EXPECT_EQ(result.mDiagnostics.at(1).mSourceFile, "second_2025.csv");
    EXPECT_EQ(result.mDiagnostics.at(1).mMessageKey, MessageKey::DuplicateRateYear);

    ASSERT_EQ(result.mTable.mFiles.size(), 1U);
    EXPECT_EQ(result.mTable.mFiles.at(0).mSourceFile.value(), "exr_2024_synthetic.csv");

    expectGood2024FileStillConverts(result.mTable);
    expectBlockedYear2025(result.mTable);
}

// Built-in rates. The built-in files are public ECB data; the literals below are values of the ECB
// download of each year (USD per 1 EUR), checked against the downloaded file.

TEST(EcbRateFileIntegrationTest, TheBuiltInRateFilesLoadWithoutErrors) {
    const auto result = loadOfficialRates(std::vector<std::filesystem::path>{});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mTable.mFiles.size(), 10U); // the years 2016 to 2025

    for (std::size_t index = 0; index < result.mTable.mFiles.size(); ++index)
    {
        const auto& file = result.mTable.mFiles.at(index);

        EXPECT_EQ(file.mYear, 2016 + static_cast<int>(index));
        EXPECT_EQ(file.mSourceFile.value(), "rates_" + std::to_string(file.mYear) + ".csv");
        EXPECT_EQ(file.mOrigin, OfficialRateOrigin::BuiltIn);
        EXPECT_FALSE(file.mReplacesBuiltIn);
    }

    // 2016 starts on the first working day, 2016-01-04, and ends on 2016-12-30.
    EXPECT_EQ(result.mTable.mFiles.front().mFirstDate, date("2016-01-04"));
    EXPECT_EQ(result.mTable.mFiles.front().mLastDate, date("2016-12-30"));
    EXPECT_EQ(result.mTable.mFiles.front().mRateCount, 8995U);

    const auto& last = result.mTable.mFiles.back();

    EXPECT_EQ(last.mFirstDate, date("2025-01-02"));
    EXPECT_EQ(last.mLastDate, date("2025-12-31"));
    EXPECT_EQ(last.mRateCount, 7650U);
}

TEST(EcbRateFileIntegrationTest, TheBuiltInRatesHaveTheEcbValuesAndTheirProvenance) {
    const auto result = loadOfficialRates(std::vector<std::filesystem::path>{});

    const OfficialRate* first = result.mTable.find(Currency::USD, date("2025-01-02"));

    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->mValue, 103'210'000); // 1.0321 at 8 decimals
    EXPECT_EQ(first->mSeriesKey, "EXR.D.USD.EUR.SP00.A");
    EXPECT_EQ(first->mSourceFile.value(), "rates_2025.csv");
    EXPECT_EQ(first->mSourceRow, 7142U); // the header is row 1
    EXPECT_EQ(first->mOrigin, OfficialRateOrigin::BuiltIn);

    const OfficialRate* lastDay = result.mTable.find(Currency::USD, date("2025-12-31"));

    ASSERT_NE(lastDay, nullptr);
    EXPECT_EQ(lastDay->mValue, 117'500'000); // 1.175 at 8 decimals

    const OfficialRate* oldest = result.mTable.find(Currency::USD, date("2016-01-04"));

    ASSERT_NE(oldest, nullptr);
    EXPECT_EQ(oldest->mValue, 108'980'000); // 1.0898

    // 2015 is not built in.
    EXPECT_EQ(result.mTable.find(Currency::USD, date("2015-12-31")), nullptr);
}

TEST(EcbRateFileIntegrationTest, ASuppliedFileReplacesTheBuiltInFileOfItsYear) {
    const auto result = loadOfficialRates({fixturePath("exr_2025_synthetic.csv")});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mTable.mFiles.size(), 10U);

    // The supplied value wins over the built-in 1.0321, with no diagnostic.
    const OfficialRate* supplied = result.mTable.find(Currency::USD, date("2025-01-02"));

    ASSERT_NE(supplied, nullptr);
    EXPECT_EQ(supplied->mValue, 125'000'000); // 1.25 of the synthetic file
    EXPECT_EQ(supplied->mSourceFile.value(), "exr_2025_synthetic.csv");
    EXPECT_EQ(supplied->mSourceRow, 4U); // header, GBP, GBP, USD
    EXPECT_EQ(supplied->mOrigin, OfficialRateOrigin::Supplied);

    // The synthetic file has no 2025-03-10, and the built-in 2025 is gone, not merged.
    EXPECT_EQ(result.mTable.find(Currency::USD, date("2025-03-10")), nullptr);

    // 2024 stays built in: USD 2024-12-31 is 1.0389.
    const OfficialRate* earlier = result.mTable.find(Currency::USD, date("2024-12-31"));

    ASSERT_NE(earlier, nullptr);
    EXPECT_EQ(earlier->mValue, 103'890'000);
    EXPECT_EQ(earlier->mOrigin, OfficialRateOrigin::BuiltIn);

    const auto& file2025 = result.mTable.mFiles.back();

    EXPECT_EQ(file2025.mYear, 2025);
    EXPECT_EQ(file2025.mOrigin, OfficialRateOrigin::Supplied);
    EXPECT_TRUE(file2025.mReplacesBuiltIn);
    EXPECT_EQ(file2025.mRateCount, 4U);
}

TEST(EcbRateFileIntegrationTest, ASuppliedFileOfAnotherYearIsAdded) {
    const auto result = loadOfficialRates({fixturePath("exr_2015_synthetic.csv")});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mTable.mFiles.size(), 11U);

    // The files are in year order, so 2015 is first.
    const auto& file2015 = result.mTable.mFiles.front();

    EXPECT_EQ(file2015.mYear, 2015);
    EXPECT_EQ(file2015.mOrigin, OfficialRateOrigin::Supplied);
    EXPECT_FALSE(file2015.mReplacesBuiltIn);

    const OfficialRate* added = result.mTable.find(Currency::USD, date("2015-12-31"));

    ASSERT_NE(added, nullptr);
    EXPECT_EQ(added->mValue, 110'000'000); // 1.1 of the synthetic file

    // The built-in 2016 is still there.
    EXPECT_NE(result.mTable.find(Currency::USD, date("2016-01-04")), nullptr);
}

TEST(EcbRateFileIntegrationTest, ABrokenSuppliedFileIsAnErrorAndTheBuiltInYearStays) {
    TemporaryDirectory directory;

    // Row 2 has OBS_STATUS "E" (estimated), which is not accepted.
    const auto badPath = directory.write(
        "exr_2025_bad.csv",
        std::string{kShortHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,E)csv" + "\n");
    const auto result = loadOfficialRates({badPath});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.at(0).mSourceFile, "exr_2025_bad.csv");
    EXPECT_EQ(result.mDiagnostics.at(0).mRowIndex, 2U);

    // The built-in 2025 is still used for the run's data.
    ASSERT_EQ(result.mTable.mFiles.size(), 10U);

    const OfficialRate* builtIn = result.mTable.find(Currency::USD, date("2025-01-02"));

    ASSERT_NE(builtIn, nullptr);
    EXPECT_EQ(builtIn->mValue, 103'210'000); // 1.0321
    EXPECT_EQ(builtIn->mOrigin, OfficialRateOrigin::BuiltIn);
}

TEST(EcbRateFileIntegrationTest, TwoSuppliedFilesOfOneYearAreBothRejectedAndTheBuiltInYearStays) {
    const auto result = loadOfficialRates(
        {fixturePath("exr_2025_synthetic.csv"), fixturePath("exr_2025_synthetic.csv")});

    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode, DiagnosticCode::DuplicateRateYear);
    EXPECT_EQ(result.mDiagnostics.at(1).mCode, DiagnosticCode::DuplicateRateYear);
    ASSERT_EQ(result.mTable.mFiles.size(), 10U);
    EXPECT_EQ(result.mTable.mFiles.back().mOrigin, OfficialRateOrigin::BuiltIn);
}

TEST(EcbRateFileIntegrationTest, TheBuiltInYearsAreSharedByEveryLoadAndNotCopiedIntoTheTable) {
    const auto first = loadOfficialRates(std::vector<std::filesystem::path>{});
    const auto second = loadOfficialRates(std::vector<std::filesystem::path>{});

    // The table owns no rate; the ten years are shared read-only.
    EXPECT_TRUE(first.mTable.mRates.empty());
    EXPECT_EQ(first.mTable.mBuiltInYears.size(), 10U);

    const OfficialRate* fromFirst = first.mTable.find(Currency::USD, date("2025-01-02"));
    const OfficialRate* fromSecond = second.mTable.find(Currency::USD, date("2025-01-02"));

    ASSERT_NE(fromFirst, nullptr);
    EXPECT_EQ(fromFirst, fromSecond); // the same object, not a copy
}

TEST(EcbRateFileIntegrationTest, ASuppliedFileOwnsItsRatesAndTheOtherYearsStayShared) {
    const auto result = loadOfficialRates({fixturePath("exr_2025_synthetic.csv")});

    EXPECT_EQ(result.mTable.mRates.size(), 4U);        // only the four rates of the supplied file
    EXPECT_EQ(result.mTable.mBuiltInYears.size(), 9U); // 2016 to 2024
}

TEST(EcbRateFileIntegrationTest, ThePublicationDaysOfTheBuiltInYearsAreTheDaysOfTheFiles) {
    const auto days =
        loadOfficialRates(std::vector<std::filesystem::path>{}).mTable.publicationDays();

    EXPECT_TRUE(days.contains(date("2016-01-04")));
    EXPECT_TRUE(days.contains(date("2022-03-10"))); // AUD 1.5109 on that day
    EXPECT_TRUE(days.contains(date("2025-12-31")));
    EXPECT_FALSE(days.contains(date("2025-01-01"))); // a holiday
    EXPECT_FALSE(days.contains(date("2025-01-04"))); // a Saturday
    EXPECT_FALSE(days.contains(date("2015-12-31"))); // not built in
}

TEST(EcbRateFileIntegrationTest, ASuppliedFileTakesTheBuiltInPublicationDaysOfItsYearAway) {
    const auto days =
        loadOfficialRates({fixturePath("exr_2025_synthetic.csv")}).mTable.publicationDays();

    EXPECT_TRUE(days.contains(date("2025-01-02")));  // supplied
    EXPECT_TRUE(days.contains(date("2025-01-03")));  // supplied
    EXPECT_FALSE(days.contains(date("2025-03-10"))); // built in 2025 is replaced
    EXPECT_TRUE(days.contains(date("2024-12-31")));  // 2024 stays built in
}

TEST(EcbRateFileIntegrationTest, ACurrencyTheEcbStoppedHasNoRateOnADayItsOtherRatesExist) {
    const auto result = loadOfficialRates(std::vector<std::filesystem::path>{});
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::RUB,
         .mTaxDate = date("2022-03-10"), // the ECB's last RUB rate is 2022-03-01
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {}},
    };

    const auto selection = selectOfficialRates(needs, result.mTable);

    EXPECT_EQ(selection.find(Currency::RUB, date("2022-03-10")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateNoRateOnDate);
}

} // namespace
