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

    // sha256sum of the fixture file.
    EXPECT_EQ(result.mTable.mFiles[0].mSha256,
              "5b5c398aaf0782b132351874977d2278a6b6492d8daa2f1ed4aa16b3ff979a58");
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

} // namespace
