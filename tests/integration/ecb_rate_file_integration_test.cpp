#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/official_rates.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>
#include <variant>

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

} // namespace
