#include "taxbroker/official_rates.hpp"

#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/exact_arithmetic.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
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

// All rates, dates and file contents in this test suite are synthetic. The header is the one of
// the ECB Data Portal `csvdata` export of the EXR dataset (32 columns).
constexpr std::string_view kHeader =
    "KEY,FREQ,CURRENCY,CURRENCY_DENOM,EXR_TYPE,EXR_SUFFIX,TIME_PERIOD,OBS_VALUE,OBS_STATUS,OBS_"
    "CONF,OBS_PRE_BREAK,OBS_COM,TIME_FORMAT,BREAKS,COLLECTION,COMPILING_ORG,DISS_ORG,DOM_SER_IDS,"
    "PUBL_ECB,PUBL_MU,PUBL_PUBLIC,UNIT_INDEX_BASE,COMPILATION,COVERAGE,DECIMALS,NAT_TITLE,SOURCE_"
    "AGENCY,SOURCE_PUB,TITLE,TITLE_COMPL,UNIT,UNIT_MULT";

Date date(std::string_view aText) {
    return *parseCalendarDate(aText);
}

/// A path under the system temp directory that no other test of this run uses.
std::filesystem::path uniqueDirectoryPath() {
    static std::atomic<unsigned> counter{0};

    return std::filesystem::temp_directory_path() /
           ("taxbroker_ecb_rates_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(counter++));
}

/// A new empty directory under the system temp directory, removed with the object.
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

    [[nodiscard]] const std::filesystem::path& path() const {
        return mPath;
    }

    /// Writes the bytes exactly as given and returns the path of the new file.
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

TEST(EcbRateFileTest, LoadsACoveredPairWithFullProvenance) {
    TemporaryDirectory directory;

    // TITLE_COMPL is quoted and contains commas; the row has all 32 columns of the ECB export.
    const auto path = directory.write(
        "exr_2025.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mTable.mRates.size(), 1U);

    const OfficialRate* rate = result.mTable.find("USD", date("2025-01-02"));

    ASSERT_NE(rate, nullptr);
    EXPECT_EQ(rate->mValue, 125'000'000); // 1.25 at 8 decimals: 1 EUR = 1.25 USD
    EXPECT_EQ(rate->mCurrencyCode, "USD");
    EXPECT_EQ(rate->mSeriesKey, "EXR.D.USD.EUR.SP00.A");
    EXPECT_EQ(rate->mSourceFile.value(), "exr_2025.csv");
    EXPECT_EQ(rate->mSourceRow, 2U); // the header is row 1
    EXPECT_EQ(rate->mSource, OfficialRateSource::EcbReferenceRateFile);
    EXPECT_EQ(rate->mFileSha256,
              "477fc33becc38dd61cc8e9a98d20cfe222f93d6056f13394eb2eae9192498f9a"); // sha256sum of
                                                                                   // the file

    // 10.0000 USD / 1.25 = 8.0000 EUR
    const auto converted =
        convertMoney(100'000, rate->mValue, ExchangeRateDirection::ForeignPerEur);

    ASSERT_TRUE(std::holds_alternative<Money>(converted));
    EXPECT_EQ(std::get<Money>(converted), 80'000);

    ASSERT_EQ(result.mTable.mFiles.size(), 1U);
    EXPECT_EQ(result.mTable.mFiles[0].mSourceFile.value(), "exr_2025.csv");
    EXPECT_EQ(result.mTable.mFiles[0].mSha256,
              "477fc33becc38dd61cc8e9a98d20cfe222f93d6056f13394eb2eae9192498f9a");
    EXPECT_EQ(result.mTable.mFiles[0].mYear, 2025);
    EXPECT_EQ(result.mTable.mFiles[0].mRateCount, 1U);
}

// A *LargeInputTest suite takes minutes under Valgrind. CI runs it on every push to main, and on a
// pull request only when a related file changes (`scripts/valgrind.sh --part large`).
TEST(EcbRateFileLargeInputTest, LoadsAFileLargerThanTheReaderChunk) {
    TemporaryDirectory directory;

    // The CSV reader reads 10 MB at a time. Each row has a 1,000,000-character TITLE, so the
    // file is 11,002,213 bytes and the last rows are in the second chunk.
    const std::string longTitle(1'000'000, 'x');
    const std::string titleCompl =
        R"csv("ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)")csv";
    const auto path = directory.write(
        "exr_2025.csv",
        std::string{kHeader} + "\n" +
            "EXR.D.AUD.EUR.SP00.A,D,AUD,EUR,SP00,A,2025-01-02,1.5,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,"
            "4F0,," +
            longTitle + "," + titleCompl + ",AUD,0\n" +
            "EXR.D.CAD.EUR.SP00.A,D,CAD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,"
            ","
            "4F0,," +
            longTitle + "," + titleCompl + ",CAD,0\n" +
            "EXR.D.CHF.EUR.SP00.A,D,CHF,EUR,SP00,A,2025-01-02,0.75,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,"
            ","
            "4F0,," +
            longTitle + "," + titleCompl + ",CHF,0\n" +
            "EXR.D.CZK.EUR.SP00.A,D,CZK,EUR,SP00,A,2025-01-02,25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,"
            "4F0,," +
            longTitle + "," + titleCompl + ",CZK,0\n" +
            "EXR.D.DKK.EUR.SP00.A,D,DKK,EUR,SP00,A,2025-01-02,7.5,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,"
            "4F0,," +
            longTitle + "," + titleCompl + ",DKK,0\n" +
            "EXR.D.GBP.EUR.SP00.A,D,GBP,EUR,SP00,A,2025-01-02,0.5,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,"
            "4F0,," +
            longTitle + "," + titleCompl + ",GBP,0\n" +
            "EXR.D.JPY.EUR.SP00.A,D,JPY,EUR,SP00,A,2025-01-02,150,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,"
            "4F0,," +
            longTitle + "," + titleCompl + ",JPY,0\n" +
            "EXR.D.NOK.EUR.SP00.A,D,NOK,EUR,SP00,A,2025-01-02,11.5,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,"
            ","
            "4F0,," +
            longTitle + "," + titleCompl + ",NOK,0\n" +
            "EXR.D.PLN.EUR.SP00.A,D,PLN,EUR,SP00,A,2025-01-02,4.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,"
            ","
            "4F0,," +
            longTitle + "," + titleCompl + ",PLN,0\n" +
            "EXR.D.SEK.EUR.SP00.A,D,SEK,EUR,SP00,A,2025-01-02,11.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,"
            "4,"
            ",4F0,," +
            longTitle + "," + titleCompl + ",SEK,0\n" +
            "EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.125,A,F,,,P1D,,A,,,,,,,99Q1=100,,,"
            "4,"
            ",4F0,," +
            longTitle + "," + titleCompl + ",USD,0\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mTable.mRates.size(), 11U);

    const OfficialRate* rate = result.mTable.find("USD", date("2025-01-02"));

    ASSERT_NE(rate, nullptr);
    EXPECT_EQ(rate->mValue, 112'500'000); // 1.125 at 8 decimals
    EXPECT_EQ(rate->mSourceRow, 12U);     // the header is row 1, USD is the 11th rate

    ASSERT_EQ(result.mTable.mFiles.size(), 1U);
    EXPECT_EQ(result.mTable.mFiles[0].mSha256,
              "0fa1571019a34cf119a81a668ba96dbecff03588178e5de85fdaf3703e0e9c7a"); // sha256sum of
                                                                                   // the file
    EXPECT_EQ(result.mTable.mFiles[0].mRateCount, 11U);
}

TEST(EcbRateFileTest, LoadsSeveralCurrenciesAndKeepsEightDecimals) {
    TemporaryDirectory directory;

    const auto path = directory.write(
        "exr_2025.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.NOK.EUR.SP00.A,D,NOK,EUR,SP00,A,2025-01-02,11.5,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",NOK,0)csv" +
            "\n" +
            R"csv(EXR.D.JPY.EUR.SP00.A,D,JPY,EUR,SP00,A,2025-01-02,163.12345678,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",JPY,0)csv" +
            "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mTable.mRates.size(), 3U);

    const OfficialRate* krone = result.mTable.find("NOK", date("2025-01-02"));
    const OfficialRate* yen = result.mTable.find("JPY", date("2025-01-02"));
    const OfficialRate* dollar = result.mTable.find("USD", date("2025-01-02"));

    ASSERT_NE(krone, nullptr);
    ASSERT_NE(yen, nullptr);
    ASSERT_NE(dollar, nullptr);
    EXPECT_EQ(krone->mValue, 1'150'000'000); // 11.5
    EXPECT_EQ(krone->mSourceRow, 2U);
    EXPECT_EQ(yen->mValue, 16'312'345'678); // 163.12345678, all 8 decimals kept
    EXPECT_EQ(yen->mSourceRow, 3U);
    EXPECT_EQ(dollar->mValue, 125'000'000);
    EXPECT_EQ(dollar->mSourceRow, 4U);
}

TEST(EcbRateFileTest, FindsOnlyTheExactCurrencyAndDate) {
    TemporaryDirectory directory;

    // 2025-01-02 is a Thursday and 2025-01-06 a Monday; the file has no row for the days between.
    const auto path = directory.write(
        "exr_2025.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-06,1.3,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    ASSERT_TRUE(result.mDiagnostics.empty());
    EXPECT_NE(result.mTable.find("USD", date("2025-01-02")), nullptr);
    EXPECT_NE(result.mTable.find("USD", date("2025-01-06")), nullptr);
    EXPECT_EQ(result.mTable.find("USD", date("2025-01-03")), nullptr); // Friday, no row
    EXPECT_EQ(result.mTable.find("USD", date("2025-01-04")), nullptr); // Saturday
    EXPECT_EQ(result.mTable.find("USD", date("2025-01-05")), nullptr); // Sunday
    EXPECT_EQ(result.mTable.find("USD", date("2024-12-31")), nullptr); // before the first row
    EXPECT_EQ(result.mTable.find("USD", date("2025-01-07")), nullptr); // after the last row
    EXPECT_EQ(result.mTable.find("GBP", date("2025-01-02")), nullptr); // not in the file
    EXPECT_EQ(result.mTable.find("usd", date("2025-01-02")), nullptr); // codes are capitals
}

TEST(EcbRateFileTest, FindsByCurrencyAndNeverReturnsEurOrUnknown) {
    TemporaryDirectory directory;

    const auto path = directory.write(
        "exr_2025.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n" +
            R"csv(EXR.D.GBP.EUR.SP00.A,D,GBP,EUR,SP00,A,2025-01-02,0.83,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",GBP,0)csv" +
            "\n" +
            R"csv(EXR.D.CHF.EUR.SP00.A,D,CHF,EUR,SP00,A,2025-01-02,0.94,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",CHF,0)csv" +
            "\n" +
            R"csv(EXR.D.JPY.EUR.SP00.A,D,JPY,EUR,SP00,A,2025-01-02,163.1,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",JPY,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_NE(result.mTable.find(Currency::USD, date("2025-01-02")), nullptr);
    ASSERT_NE(result.mTable.find(Currency::GBP, date("2025-01-02")), nullptr);
    ASSERT_NE(result.mTable.find(Currency::CHF, date("2025-01-02")), nullptr);
    ASSERT_NE(result.mTable.find(Currency::JPY, date("2025-01-02")), nullptr);
    EXPECT_EQ(result.mTable.find(Currency::USD, date("2025-01-02"))->mValue, 125'000'000);
    EXPECT_EQ(result.mTable.find(Currency::GBP, date("2025-01-02"))->mValue, 83'000'000);
    EXPECT_EQ(result.mTable.find(Currency::CHF, date("2025-01-02"))->mValue, 94'000'000);
    EXPECT_EQ(result.mTable.find(Currency::JPY, date("2025-01-02"))->mValue, 16'310'000'000);
    EXPECT_EQ(result.mTable.find(Currency::EUR, date("2025-01-02")), nullptr);
    EXPECT_EQ(result.mTable.find(Currency::Unknown, date("2025-01-02")), nullptr);
    EXPECT_EQ(result.mTable.find(Currency::USD, date("2025-01-03")), nullptr);
}

TEST(EcbRateFileTest, FindsACurrencyThatIsNotOneOfTheFirstFour) {
    TemporaryDirectory directory;

    const auto path = directory.write(
        "exr_2025.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.NOK.EUR.SP00.A,D,NOK,EUR,SP00,A,2025-01-02,11.5,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",NOK,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_NE(result.mTable.find(Currency::NOK, date("2025-01-02")), nullptr);
    EXPECT_EQ(result.mTable.find(Currency::NOK, date("2025-01-02"))->mValue, 1'150'000'000); // 11.5
    EXPECT_EQ(result.mTable.find(Currency::SEK, date("2025-01-02")), nullptr); // not in the file
    EXPECT_EQ(result.mTable.find(Currency::EUR, date("2025-01-02")), nullptr);
}

TEST(EcbRateFileTest, ReadsColumnsByNameInAnyOrderAndIgnoresOtherColumns) {
    TemporaryDirectory directory;

    // The columns are in another order and the last one is not an ECB column.
    const auto path = directory.write(
        "exr_2025.csv",
        std::string{
            R"csv(OBS_VALUE,KEY,FREQ,CURRENCY,CURRENCY_DENOM,EXR_TYPE,EXR_SUFFIX,TIME_PERIOD,OBS_STATUS,EXTRA)csv"} +
            "\n" + R"csv(1.25,EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,A,anything)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_NE(result.mTable.find("USD", date("2025-01-02")), nullptr);
    EXPECT_EQ(result.mTable.find("USD", date("2025-01-02"))->mValue, 125'000'000);
}

TEST(EcbRateFileTest, AcceptsWindowsLineEnds) {
    TemporaryDirectory directory;

    const auto path = directory.write(
        "exr_2025.csv",
        std::string{kHeader} + "\r\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\r\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-03,1.26,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\r\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mTable.mRates.size(), 2U);
    EXPECT_EQ(result.mTable.find("USD", date("2025-01-03"))->mValue, 126'000'000);
}

TEST(EcbRateFileTest, LoadsOneFilePerYearForSeveralYears) {
    TemporaryDirectory directory;

    const auto path2024 = directory.write(
        "exr_2024.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2024-12-31,1.1,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto path2025 = directory.write(
        "exr_2025.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");

    const auto result = loadEcbRateFiles({path2025, path2024});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mTable.mFiles.size(), 2U);
    EXPECT_EQ(result.mTable.mFiles[0].mYear, 2025); // in the order of the arguments
    EXPECT_EQ(result.mTable.mFiles[1].mYear, 2024);
    EXPECT_EQ(result.mTable.find("USD", date("2024-12-31"))->mValue, 110'000'000);
    EXPECT_EQ(result.mTable.find("USD", date("2024-12-31"))->mSourceFile.value(), "exr_2024.csv");
    EXPECT_EQ(result.mTable.find("USD", date("2025-01-02"))->mValue, 125'000'000);
    EXPECT_EQ(result.mTable.find("USD", date("2025-01-02"))->mSourceFile.value(), "exr_2025.csv");
}

TEST(EcbRateFileTest, RejectsTheDataOnlyDownloadWithoutTheStatusColumn) {
    TemporaryDirectory directory;

    // The detail=dataonly download has 8 columns and no OBS_STATUS.
    const auto path = directory.write(
        "bad.csv",
        std::string{
            R"csv(KEY,FREQ,CURRENCY,CURRENCY_DENOM,EXR_TYPE,EXR_SUFFIX,TIME_PERIOD,OBS_VALUE)csv"} +
            "\n" + R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25)csv" + "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::nullopt);
    EXPECT_EQ(diagnostic.mField, std::nullopt);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidHeaderRateFileColumns);
    EXPECT_EQ(
        diagnostic.mMessageParams,
        (std::vector<MessageParam>{numberParam("count", 1U), textParam("columns", "OBS_STATUS")}));
    EXPECT_EQ(diagnostic.mMessage,
              "The file is not an ECB exchange-rate export. Required columns are "
              "missing: 1 (OBS_STATUS). No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAHeaderWithARequiredColumnTwice) {
    TemporaryDirectory directory;

    // OBS_VALUE is named twice, so it is unclear which column holds the rate.
    const auto path = directory.write(
        "bad.csv",
        std::string{
            R"csv(KEY,FREQ,CURRENCY,CURRENCY_DENOM,EXR_TYPE,EXR_SUFFIX,TIME_PERIOD,OBS_VALUE,OBS_STATUS,OBS_VALUE)csv"} +
            "\n" + R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,9.99)csv" + "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::nullopt);
    EXPECT_EQ(diagnostic.mField, std::nullopt);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidHeaderRateFileDuplicateColumn);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{textParam("column", "OBS_VALUE")}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column OBS_VALUE appears more than once in the header, so it is not clear "
              "which one to read. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAnEmptyFile) {
    TemporaryDirectory directory;

    // An empty file has no header, so every required column is missing.
    const auto path = directory.write("bad.csv", "");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::nullopt);
    EXPECT_EQ(diagnostic.mField, std::nullopt);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidHeaderRateFileColumns);
    EXPECT_EQ(
        diagnostic.mMessageParams,
        (std::vector<MessageParam>{numberParam("count", 9U),
                                   textParam("columns",
                                             "KEY, FREQ, CURRENCY, CURRENCY_DENOM, EXR_TYPE, "
                                             "EXR_SUFFIX, TIME_PERIOD, OBS_VALUE, OBS_STATUS")}));
    EXPECT_EQ(diagnostic.mMessage,
              "The file is not an ECB exchange-rate export. Required columns are "
              "missing: 9 (KEY, FREQ, CURRENCY, CURRENCY_DENOM, EXR_TYPE, EXR_SUFFIX, "
              "TIME_PERIOD, OBS_VALUE, OBS_STATUS). No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsABinaryFile) {
    TemporaryDirectory directory;

    // Bytes that are not text: no header column can be found.
    const auto path = directory.write("bad.csv", std::string{"\x00\x01\x02\xff\xfe binary", 12});
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics[0].mMessageKey, MessageKey::InvalidHeaderRateFileColumns);
}

TEST(EcbRateFileTest, RejectsARowWithTheWrongFieldCount) {
    TemporaryDirectory directory;

    // The row has 31 fields: UNIT_MULT is missing.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::FieldCountMismatch);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::nullopt);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::FieldCountMismatchRateFile);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{numberParam("expected", 32U), numberParam("found", 31U)}));
    EXPECT_EQ(diagnostic.mMessage,
              "A row has a different number of fields than the header (expected 32, "
              "found 31). No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAnotherFrequency) {
    TemporaryDirectory directory;

    // FREQ is M (monthly), not D.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,M,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"FREQ"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateFieldExpected);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{textParam("field", "FREQ"), textParam("expected", "D")}));
    EXPECT_EQ(diagnostic.mMessage, "The column FREQ must be D in every row. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAnotherDenominatorCurrency) {
    TemporaryDirectory directory;

    // CURRENCY_DENOM is USD, not EUR.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,USD,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"CURRENCY_DENOM"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateFieldExpected);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{textParam("field", "CURRENCY_DENOM"),
                                         textParam("expected", "EUR")}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column CURRENCY_DENOM must be EUR in every row. No rates were "
              "loaded.");
}

TEST(EcbRateFileTest, RejectsAnotherExchangeRateType) {
    TemporaryDirectory directory;

    // EXR_TYPE is SP01, not SP00.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP01,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"EXR_TYPE"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateFieldExpected);
    EXPECT_EQ(
        diagnostic.mMessageParams,
        (std::vector<MessageParam>{textParam("field", "EXR_TYPE"), textParam("expected", "SP00")}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column EXR_TYPE must be SP00 in every row. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAnotherSeriesSuffix) {
    TemporaryDirectory directory;

    // EXR_SUFFIX is E (end of period), not A.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,E,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"EXR_SUFFIX"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateFieldExpected);
    EXPECT_EQ(
        diagnostic.mMessageParams,
        (std::vector<MessageParam>{textParam("field", "EXR_SUFFIX"), textParam("expected", "A")}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column EXR_SUFFIX must be A in every row. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsALowercaseCurrencyCode) {
    TemporaryDirectory directory;

    // CURRENCY is usd, not USD.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,usd,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",usd,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"CURRENCY"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateCurrency);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column CURRENCY must be a three-letter code in capital letters "
              "other than EUR. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsEurAsTheCurrency) {
    TemporaryDirectory directory;

    // CURRENCY is EUR; EUR has no rate.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.EUR.EUR.SP00.A,D,EUR,EUR,SP00,A,2025-01-02,1,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",EUR,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"CURRENCY"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateCurrency);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column CURRENCY must be a three-letter code in capital letters "
              "other than EUR. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAKeyThatDoesNotMatchTheCurrency) {
    TemporaryDirectory directory;

    // The USD row has the key of GBP.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.GBP.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"KEY"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateFieldExpected);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{textParam("field", "KEY"),
                                         textParam("expected", "EXR.D.USD.EUR.SP00.A")}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column KEY must be EXR.D.USD.EUR.SP00.A in every row. No rates "
              "were loaded.");
}

TEST(EcbRateFileTest, RejectsADateThatDoesNotExist) {
    TemporaryDirectory directory;

    // TIME_PERIOD is 2025-02-30.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-02-30,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"TIME_PERIOD"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateDate);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column TIME_PERIOD must be a calendar date in the form YYYY-MM-DD. "
              "No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsADateInAnotherFormat) {
    TemporaryDirectory directory;

    // TIME_PERIOD is 02/01/2025.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,02/01/2025,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"TIME_PERIOD"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateDate);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column TIME_PERIOD must be a calendar date in the form YYYY-MM-DD. "
              "No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAnObservationStatusOtherThanNormal) {
    TemporaryDirectory directory;

    // OBS_STATUS is E (estimated); only A is accepted.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,E,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"OBS_STATUS"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateFieldExpected);
    EXPECT_EQ(
        diagnostic.mMessageParams,
        (std::vector<MessageParam>{textParam("field", "OBS_STATUS"), textParam("expected", "A")}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column OBS_STATUS must be A in every row. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAnEmptyValue) {
    TemporaryDirectory directory;

    // OBS_VALUE is empty.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"OBS_VALUE"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::MissingFieldRateValue);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage, "The column OBS_VALUE is empty. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAZeroValue) {
    TemporaryDirectory directory;

    // OBS_VALUE is 0.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,0,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"OBS_VALUE"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateValue);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column OBS_VALUE must be a positive number with at most eight "
              "decimals. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsANegativeValue) {
    TemporaryDirectory directory;

    // OBS_VALUE is -1.1.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,-1.1,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"OBS_VALUE"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateValue);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column OBS_VALUE must be a positive number with at most eight "
              "decimals. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAValueWithNineDecimals) {
    TemporaryDirectory directory;

    // OBS_VALUE is 1.123456789, nine decimals.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.123456789,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"OBS_VALUE"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateValue);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column OBS_VALUE must be a positive number with at most eight "
              "decimals. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAValueWithADecimalComma) {
    TemporaryDirectory directory;

    // OBS_VALUE is the quoted text 1,25.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,"1,25",A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"OBS_VALUE"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateValue);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column OBS_VALUE must be a positive number with at most eight "
              "decimals. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAValueThatIsNotANumber) {
    TemporaryDirectory directory;

    // OBS_VALUE is abc.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,abc,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"OBS_VALUE"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateValue);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The column OBS_VALUE must be a positive number with at most eight "
              "decimals. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsDatesFromMoreThanOneYear) {
    TemporaryDirectory directory;

    // The first row is in 2025 and the second row in 2026.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-12-31,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2026-01-02,1.26,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::MixedRateYears);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{3});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"TIME_PERIOD"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::MixedRateYears);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{numberParam("firstYear", 2025U),
                                         numberParam("foundYear", 2026U)}));
    EXPECT_EQ(diagnostic.mMessage,
              "A rate file must cover one calendar year: it starts in 2025 and has a "
              "row from 2026. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsTheSameCurrencyAndDateTwice) {
    TemporaryDirectory directory;

    // Rows 2 and 3 are both USD on 2025-01-02.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.26,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::DuplicateRate);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{3});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"TIME_PERIOD"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::DuplicateRate);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The file has the same currency and date more than once. No rates were "
              "loaded.");
}

TEST(EcbRateFileTest, ReportsOnlyTheFirstFaultOfAFile) {
    TemporaryDirectory directory;

    // Row 2 has FREQ M and row 3 has OBS_STATUS E; only the first fault is reported.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,M,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-03,1.25,E,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"FREQ"});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidValueRateFieldExpected);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{textParam("field", "FREQ"), textParam("expected", "D")}));
    EXPECT_EQ(diagnostic.mMessage, "The column FREQ must be D in every row. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAFileWithoutRates) {
    TemporaryDirectory directory;

    // The file has a header and no rows.
    const auto path = directory.write("bad.csv", std::string{kHeader} + "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::NoRates);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::nullopt);
    EXPECT_EQ(diagnostic.mField, std::nullopt);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::NoRates);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage, "The file has a header but no rates. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAFileWithAnUnclosedQuote) {
    TemporaryDirectory directory;

    // The quote before "ECB reference" is never closed.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic,"ECB reference exchange rate, Synthetic currency/Euro,USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::nullopt);
    EXPECT_EQ(diagnostic.mField, std::nullopt);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::ParseErrorUnclosedQuote);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The CSV file has an unclosed quote; it may be cut off or edited. No "
              "rows were imported.");
}

TEST(EcbRateFileTest, RejectsTwoRowsMergedByAQuoteThatClosesOnTheNextLine) {
    TemporaryDirectory directory;

    // The quote count is even, but the quote before "Title of USD" closes only after "Title of
    // GBP" on the next line. The reader then makes one record of 32 fields from both lines: the
    // USD fields up to TITLE, one TITLE_COMPL with a line break, and UNIT and UNIT_MULT of GBP.
    const auto path = directory.write(
        "bad.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic,"Title of USD,USD,0)csv" +
            "\n" +
            R"csv(EXR.D.GBP.EUR.SP00.A,D,GBP,EUR,SP00,A,2025-01-02,0.85,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic,Title of GBP",GBP,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({path});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(diagnostic.mSourceFile, "bad.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::nullopt);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::ParseErrorUnclosedQuote);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The CSV file has an unclosed quote; it may be cut off or edited. No "
              "rows were imported.");
}

TEST(EcbRateFileTest, RejectsAFileThatDoesNotExist) {
    TemporaryDirectory directory;

    const auto result = loadEcbRateFiles({directory.path() / "missing.csv"});

    EXPECT_TRUE(result.mTable.mRates.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(diagnostic.mSourceFile, "missing.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::nullopt);
    EXPECT_EQ(diagnostic.mField, std::nullopt);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::ParseErrorUnreadableRateFile);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The rate file could not be opened or read. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsADirectoryAndKeepsTheOtherFile) {
    TemporaryDirectory directory;

    // A folder named like a rate file cannot be opened as a file; the other file still loads.
    std::filesystem::create_directory(directory.path() / "exr_2024.csv");
    const auto goodPath = directory.write(
        "exr_2025.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({directory.path() / "exr_2024.csv", goodPath});

    ASSERT_EQ(result.mTable.mRates.size(), 1U);
    EXPECT_NE(result.mTable.find("USD", date("2025-01-02")), nullptr);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(diagnostic.mSourceFile, "exr_2024.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::nullopt);
    EXPECT_EQ(diagnostic.mField, std::nullopt);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::ParseErrorUnreadableRateFile);
    EXPECT_EQ(diagnostic.mMessage,
              "The rate file could not be opened or read. No rates were loaded.");
}

TEST(EcbRateFileLargeInputTest, RejectsARowLongerThanTheReaderLimitAndKeepsTheOtherFile) {
    TemporaryDirectory directory;

    // The CSV reader throws on a row longer than two 10 MB chunks. This row has a
    // 21,000,000-character TITLE; the 2025 file still loads.
    const std::string longTitle(21'000'000, 'x');
    const auto badPath = directory.write(
        "exr_2024.csv",
        std::string{kHeader} + "\n" +
            "EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2024-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,"
            ","
            "4F0,," +
            longTitle +
            R"csv(,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto goodPath = directory.write(
        "exr_2025.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto result = loadEcbRateFiles({badPath, goodPath});

    ASSERT_EQ(result.mTable.mRates.size(), 1U);
    EXPECT_NE(result.mTable.find("USD", date("2025-01-02")), nullptr);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(diagnostic.mSourceFile, "exr_2024.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::nullopt);
    EXPECT_EQ(diagnostic.mField, std::nullopt);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::ParseErrorUnreadableRateFile);
    EXPECT_EQ(diagnostic.mMessage,
              "The rate file could not be opened or read. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsAnEmptyPathWithoutAFileName) {
    // An empty path has no file name to show.
    const auto result = loadEcbRateFiles({std::filesystem::path{}});

    EXPECT_TRUE(result.mTable.mRates.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(diagnostic.mSourceFile, "");
    EXPECT_EQ(diagnostic.mRowIndex, std::nullopt);
    EXPECT_EQ(diagnostic.mField, std::nullopt);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::ParseErrorUnreadableRateFile);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{}));
    EXPECT_EQ(diagnostic.mMessage,
              "The rate file could not be opened or read. No rates were loaded.");
}

TEST(EcbRateFileTest, RejectsBothFilesOfTheSameYearAndKeepsOtherYears) {
    TemporaryDirectory directory;

    // Two files cover 2025, so both are rejected; the 2024 file still loads.
    const auto firstPath = directory.write(
        "first_2025.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto secondPath = directory.write(
        "second_2025.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-03,1.26,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto otherPath = directory.write(
        "exr_2024.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2024-12-31,1.1,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");

    const auto result = loadEcbRateFiles({firstPath, secondPath, otherPath});

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::DuplicateRateYear);
    EXPECT_EQ(diagnostic.mSourceFile, "first_2025.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::nullopt);
    EXPECT_EQ(diagnostic.mField, std::nullopt);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::DuplicateRateYear);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{numberParam("year", 2025U)}));
    EXPECT_EQ(diagnostic.mMessage, "More than one rate file covers 2025. None of them was loaded.");

    const auto& second = result.mDiagnostics[1];

    EXPECT_EQ(second.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(second.mCode, DiagnosticCode::DuplicateRateYear);
    EXPECT_EQ(second.mSourceFile, "second_2025.csv");
    EXPECT_EQ(second.mRowIndex, std::nullopt);
    EXPECT_EQ(second.mField, std::nullopt);
    EXPECT_EQ(second.mMessageKey, MessageKey::DuplicateRateYear);
    EXPECT_EQ(second.mMessageParams, (std::vector<MessageParam>{numberParam("year", 2025U)}));
    EXPECT_EQ(second.mMessage, "More than one rate file covers 2025. None of them was loaded.");

    ASSERT_EQ(result.mTable.mFiles.size(), 1U);
    EXPECT_EQ(result.mTable.mFiles[0].mSourceFile.value(), "exr_2024.csv");
    EXPECT_EQ(result.mTable.mRates.size(), 1U);
    EXPECT_NE(result.mTable.find("USD", date("2024-12-31")), nullptr);
    EXPECT_EQ(result.mTable.find("USD", date("2025-01-02")), nullptr);
    EXPECT_EQ(result.mTable.find("USD", date("2025-01-03")), nullptr);
}

TEST(EcbRateFileTest, RejectsTwoIdenticalFilesOfTheSameYear) {
    TemporaryDirectory directory;

    // The same bytes under two names are still two files for 2025.
    const auto firstPath = directory.write(
        "copy_a.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto secondPath = directory.write(
        "copy_b.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");

    const auto result = loadEcbRateFiles({firstPath, secondPath});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::DuplicateRateYear);
    EXPECT_EQ(result.mDiagnostics[0].mSourceFile, "copy_a.csv");
    EXPECT_EQ(result.mDiagnostics[1].mCode, DiagnosticCode::DuplicateRateYear);
    EXPECT_EQ(result.mDiagnostics[1].mSourceFile, "copy_b.csv");
}

TEST(EcbRateFileTest, LoadsAGoodFileBesideARejectedFileOfTheSameYear) {
    TemporaryDirectory directory;

    // The first file has OBS_STATUS E; the second is fine and for the same dates.
    const auto badPath = directory.write(
        "bad_2025.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,E,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");
    const auto goodPath = directory.write(
        "good_2025.csv",
        std::string{kHeader} + "\n" +
            R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2025-01-02,1.25,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
            "\n");

    const auto result = loadEcbRateFiles({badPath, goodPath});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mSourceFile, "bad_2025.csv");
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InvalidValue);
    ASSERT_EQ(result.mTable.mFiles.size(), 1U);
    EXPECT_EQ(result.mTable.mFiles[0].mSourceFile.value(), "good_2025.csv");
    ASSERT_NE(result.mTable.find("USD", date("2025-01-02")), nullptr);
    EXPECT_EQ(result.mTable.find("USD", date("2025-01-02"))->mSourceFile.value(), "good_2025.csv");
}

TEST(EcbRateFileTest, ReturnsNoRateForACurrencyValueOutsideTheEnum) {
    const OfficialRateTable table;

    EXPECT_EQ(table.find(static_cast<Currency>(999), date("2025-01-02")), nullptr);
}

TEST(EcbRateFileTest, LoadsNothingWithoutFiles) {
    const auto result = loadEcbRateFiles({});

    EXPECT_TRUE(result.mTable.mRates.empty());
    EXPECT_TRUE(result.mTable.mFiles.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

} // namespace
