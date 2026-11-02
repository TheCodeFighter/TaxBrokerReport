#include "processors/official_rate_selection.hpp"
#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/official_rates.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <filesystem>
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

} // namespace
