#include "processors/official_rate_selection.hpp"

#include "taxbroker/currency.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace taxbroker;

// All rates, dates and sources in this test suite are synthetic.

Date date(std::string_view aText) {
    return *parseCalendarDate(aText);
}

/// Marks the rate file of a year as loaded. It starts on aFirstDate (2 January when not given)
/// and ends on aLastDate (31 December when not given).
void addFile(OfficialRateTable& aTable,
             int aYear,
             std::string_view aLastDate = "",
             std::string_view aFirstDate = "") {
    const std::string firstDate =
        aFirstDate.empty() ? std::to_string(aYear) + "-01-02" : std::string{aFirstDate};
    const std::string lastDate =
        aLastDate.empty() ? std::to_string(aYear) + "-12-31" : std::string{aLastDate};

    aTable.mFiles.push_back(OfficialRateFile{.mYear = aYear,
                                             .mFirstDate = date(firstDate),
                                             .mLastDate = date(lastDate)});
}

/// Adds one rate; aValue is foreign units per 1 EUR at 8 decimals.
void addRate(OfficialRateTable& aTable,
             std::string_view aCode,
             std::string_view aDate,
             ExchangeRate aValue) {
    aTable.mRates[{std::string{aCode}, date(aDate)}] =
        OfficialRate{.mCurrencyCode = std::string{aCode},
                     .mObservationDate = date(aDate),
                     .mValue = aValue,
                     .mSourceFile = SourceFilename::fromPath("exr_synthetic.csv"),
                     .mSourceRow = 2,
                     .mSource = OfficialRateSource::EcbReferenceRateFile,
                     .mOrigin = OfficialRateOrigin::Supplied};
}

SourceReference source(std::size_t aRow, std::string aTransactionId) {
    return SourceReference{
        .mBroker = Broker::TradeRepublic,
        .mFilename = SourceFilename::fromPath("history.csv"),
        .mSourceRow = aRow,
        .mTransactionId = std::move(aTransactionId),
    };
}

template <typename T> T requireValue(const NumericResult<T>& aResult) {
    EXPECT_TRUE(std::holds_alternative<T>(aResult));

    return std::get<T>(aResult);
}

TEST(OfficialRateSelectionTest, EurNeedsGetNoLookupAndNoDiagnostic) {
    OfficialRateTable table;
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::EUR,
         .mTaxDate = date("2025-03-08"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_TRUE(selection.mSelected.empty());
    EXPECT_TRUE(selection.mDiagnostics.empty());
}

TEST(OfficialRateSelectionTest, UnknownCurrencyNeedIsAProgrammingError) {
    OfficialRateTable table;
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::Unknown,
         .mTaxDate = date("2025-03-03"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    EXPECT_THROW((void)selectOfficialRates(needs, table), std::logic_error);
}

TEST(OfficialRateSelectionTest, AMissingPairKeepsItsErrorAndAFoundPairHasNone) {
    OfficialRateTable table;
    addFile(table, 2025);
    addRate(table, "USD", "2025-03-03", 125'000'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-03"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
        // No file of 2024 is loaded.
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2024-03-04"),
         .mUse = RateUse::PurchasePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(3, "b")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mMissing.size(), 1U);
    EXPECT_EQ(selection.findMissing(Currency::USD, date("2025-03-03")), nullptr);

    const auto* missing = selection.findMissing(Currency::USD, date("2024-03-04"));

    ASSERT_NE(missing, nullptr);
    EXPECT_EQ(missing->mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(missing->mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(missing->mMessage, selection.mDiagnostics.at(0).mMessage);
    EXPECT_EQ(missing->mMessageKey, MessageKey::MissingExchangeRateNoRateFile);
    EXPECT_EQ(selection.findMissing(Currency::EUR, date("2024-03-04")), nullptr);
}

TEST(OfficialRateSelectionTest, AMissingFileIsRecordedWithItsYear) {
    OfficialRateTable table;
    addFile(table, 2025);
    addRate(table, "USD", "2025-03-03", 125'000'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-03"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
        // No file of 2024 is loaded.
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2024-06-10"),
         .mUse = RateUse::PurchasePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(3, "b")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_EQ(selection.mMissingFiles.size(), 1U);
    EXPECT_EQ(selection.findMissingFile(Currency::USD, date("2025-03-03")), nullptr);

    const auto* need = selection.findMissingFile(Currency::USD, date("2024-06-10"));

    ASSERT_NE(need, nullptr);
    EXPECT_EQ(need->mYear, 2024);
    EXPECT_EQ(need->mProblem, RateFileProblem::NotLoaded);
    EXPECT_EQ(selection.findMissingFile(Currency::EUR, date("2024-06-10")), nullptr);
}

TEST(OfficialRateSelectionTest, ALookBackIntoAMissingYearNeedsThatYearsFile) {
    OfficialRateTable table;
    addFile(table, 2025);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-01-01"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    // 2025-01-01 has no ECB rate, so the day before (2024-12-31) is needed: the file of 2024.
    const auto* need = selection.findMissingFile(Currency::USD, date("2025-01-01"));

    ASSERT_NE(need, nullptr);
    EXPECT_EQ(need->mYear, 2024);
    EXPECT_EQ(need->mProblem, RateFileProblem::NotLoaded);

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey, MessageKey::MissingExchangeRateNoRateFile);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams.at(2), numberParam("year", 2024));
}

TEST(OfficialRateSelectionTest, AFileThatEndsEarlyIsRecorded) {
    OfficialRateTable table;
    addFile(table, 2025, "2025-06-30");
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-09-10"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    const auto* need = selection.findMissingFile(Currency::USD, date("2025-09-10"));

    ASSERT_NE(need, nullptr);
    EXPECT_EQ(need->mYear, 2025);
    EXPECT_EQ(need->mProblem, RateFileProblem::EndsEarly);
}

TEST(OfficialRateSelectionTest, AMissingRateOnTheDateNeedsNoFile) {
    OfficialRateTable table;
    addFile(table, 2025);
    addRate(table, "GBP", "2025-03-10", 85'000'000); // The ECB published on this day, but not USD.
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-10"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateNoRateOnDate);
    EXPECT_NE(selection.findMissing(Currency::USD, date("2025-03-10")), nullptr);
    EXPECT_EQ(selection.findMissingFile(Currency::USD, date("2025-03-10")), nullptr);
    EXPECT_TRUE(selection.mMissingFiles.empty());
}

TEST(OfficialRateSelectionTest, FindsTheRateOfTheExactDateWithItsProvenance) {
    OfficialRateTable table;
    addFile(table, 2025);
    addRate(table, "USD", "2025-03-03", 125'000'000); // 1 EUR = 1.25 USD
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-03"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_TRUE(selection.mDiagnostics.empty());

    const auto* rate = selection.find(Currency::USD, date("2025-03-03"));

    ASSERT_NE(rate, nullptr);
    EXPECT_EQ(rate->mValue, 125'000'000);
    EXPECT_EQ(rate->mObservationDate, date("2025-03-03"));
    EXPECT_EQ(rate->mSourceFile, SourceFilename::fromPath("exr_synthetic.csv"));
    EXPECT_EQ(rate->mSourceRow, 2U);
}

TEST(OfficialRateSelectionTest, TwoNeedsOfOnePairGiveOneSelectedRateAndNoDiagnostic) {
    OfficialRateTable table;
    addFile(table, 2025);
    addRate(table, "USD", "2025-03-03", 125'000'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-03"),
         .mUse = RateUse::IncomeGross,
         .mReport = ReportType::Dividends,
         .mSources = {source(2, "a")}},
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-03"),
         .mUse = RateUse::ForeignTax,
         .mReport = ReportType::Dividends,
         .mSources = {source(3, "b")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_EQ(selection.mSelected.size(), 1U);
    EXPECT_TRUE(selection.mDiagnostics.empty());
}

TEST(OfficialRateSelectionTest, ConvertsMoneyAtAnOfficialRate) {
    const OfficialRate rate{.mValue = 125'000'000}; // 1 EUR = 1.25 USD

    // 10.0000 USD / 1.25 = 8.0000 EUR
    EXPECT_EQ(requireValue(convertOfficialMoney(100'000, rate)), 80'000);
    // 123.4567 USD / 1.25 = 98.76536 EUR, rounded to 98.7654 EUR
    EXPECT_EQ(requireValue(convertOfficialMoney(1'234'567, rate)), 987'654);
}

TEST(OfficialRateSelectionTest, RoundsAnExactHalfStepAwayFromZero) {
    const OfficialRate rate{.mValue = 200'000'000}; // 1 EUR = 2 USD

    // 0.0001 / 2 = 0.00005, rounded away from zero to 0.0001
    EXPECT_EQ(requireValue(convertOfficialMoney(1, rate)), 1);
    // -0.0001 / 2 = -0.00005, rounded away from zero to -0.0001
    EXPECT_EQ(requireValue(convertOfficialMoney(-1, rate)), -1);
}

TEST(OfficialRateSelectionTest, ConvertsAUnitPriceAtAnOfficialRate) {
    const OfficialRate rate{.mValue = 108'450'000}; // 1 EUR = 1.0845 USD

    // 10.00000000 USD / 1.0845 = 9.220839096... EUR, rounded to 9.22083910 EUR
    EXPECT_EQ(requireValue(convertOfficialUnitPrice(1'000'000'000, rate)), 922'083'910);
}

TEST(OfficialRateSelectionTest, ReportsOneNoRateOnDateErrorWhenTheLoadedFileHasNoRateNearby) {
    OfficialRateTable table;
    addFile(table, 2025);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-08"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_EQ(selection.find(Currency::USD, date("2025-03-08")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);

    const auto& diagnostic = selection.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::MissingExchangeRateNoRateOnDate);
    EXPECT_EQ(
        diagnostic.mMessageParams,
        (std::vector<MessageParam>{textParam("currency", "USD"), textParam("date", "2025-03-08")}));
    EXPECT_EQ(diagnostic.mContext.mTaxDate, date("2025-03-08"));
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(diagnostic.mContext.mSources, (std::vector<SourceReference>{source(2, "a")}));
}

TEST(OfficialRateSelectionTest, ReportsANotLoadedFileOfTheTaxYear) {
    OfficialRateTable table;
    addFile(table, 2025);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2024-06-03"),
         .mUse = RateUse::PurchasePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey, MessageKey::MissingExchangeRateNoRateFile);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "USD"),
                                         textParam("date", "2024-06-03"),
                                         numberParam("year", 2024)}));
}

TEST(OfficialRateSelectionTest, OneErrorForAPairNeededByTwoReportsAndTwoEvents) {
    OfficialRateTable table;
    addFile(table, 2025);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-08"),
         .mUse = RateUse::IncomeGross,
         .mReport = ReportType::Dividends,
         .mSources = {source(2, "a")}},
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-08"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(5, "b")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);

    const auto& diagnostic = selection.mDiagnostics.at(0);

    // The fixed report order: capital gains before dividends.
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains, ReportType::Dividends}));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(2, "a"), source(5, "b")}));
}

TEST(OfficialRateSelectionTest, HrkAfterTheEuroDateIsReplacedByEur) {
    OfficialRateTable table;
    addFile(table, 2023);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::HRK,
         .mTaxDate = date("2023-01-02"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateReplacedByEur);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "HRK"),
                                         textParam("date", "2023-01-02"),
                                         textParam("euroDate", "2023-01-01")}));
}

TEST(OfficialRateSelectionTest, HrkOnTheLastRateDayUsesItsFileRate) {
    OfficialRateTable table;
    addFile(table, 2022);
    addRate(table, "HRK", "2022-12-30", 753'450'000); // 1 EUR = 7.5345 HRK
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::HRK,
         .mTaxDate = date("2022-12-30"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_TRUE(selection.mDiagnostics.empty());
    ASSERT_NE(selection.find(Currency::HRK, date("2022-12-30")), nullptr);
}

TEST(OfficialRateSelectionTest, BgnAfterTheEuroDateIsReplacedByEurAndNeverTakesAnEarlierRate) {
    OfficialRateTable table;
    addFile(table, 2025);
    addFile(table, 2026);
    addRate(table, "BGN", "2025-12-31", 195'583'000); // 1 EUR = 1.95583 BGN
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::BGN,
         .mTaxDate = date("2026-01-03"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_EQ(selection.find(Currency::BGN, date("2026-01-03")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateReplacedByEur);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams.at(2),
              textParam("euroDate", "2026-01-01"));
}

TEST(OfficialRateSelectionTest, BgnOnTheLastRateDayUsesItsFileRate) {
    OfficialRateTable table;
    addFile(table, 2025);
    addRate(table, "BGN", "2025-12-31", 195'583'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::BGN,
         .mTaxDate = date("2025-12-31"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_TRUE(selection.mDiagnostics.empty());
    ASSERT_NE(selection.find(Currency::BGN, date("2025-12-31")), nullptr);
}

TEST(OfficialRateSelectionTest, NoRateFartherBackThanFourDaysIsUsed) {
    OfficialRateTable table;
    addFile(table, 2025);
    // 2025-03-12 is a Wednesday. 2025-03-07 is 5 days back, 2025-03-17 is 5 days ahead.
    addRate(table, "USD", "2025-03-07", 108'570'000);
    addRate(table, "USD", "2025-03-17", 108'000'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-12"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_EQ(selection.find(Currency::USD, date("2025-03-12")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateNoRateOnDate);
}

TEST(OfficialRateSelectionTest, NeverTakesALaterRate) {
    OfficialRateTable table;
    addFile(table, 2025);
    // 2025-03-08 is a Saturday; only Monday 2025-03-10 has a rate.
    addRate(table, "USD", "2025-03-10", 108'000'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-08"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_EQ(selection.find(Currency::USD, date("2025-03-08")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
}

TEST(OfficialRateSelectionTest, DiagnosticsAreOrderedByTaxDateThenCurrencyCode) {
    OfficialRateTable table;
    addFile(table, 2025);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-08"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
        {.mCurrency = Currency::GBP,
         .mTaxDate = date("2025-03-08"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(3, "b")}},
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-01"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(4, "c")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    ASSERT_EQ(selection.mDiagnostics.size(), 3U);
    // 2025-03-01 USD, then 2025-03-08 GBP, then 2025-03-08 USD.
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams.at(0), textParam("currency", "USD"));
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams.at(1), textParam("date", "2025-03-01"));
    EXPECT_EQ(selection.mDiagnostics.at(1).mMessageParams.at(0), textParam("currency", "GBP"));
    EXPECT_EQ(selection.mDiagnostics.at(1).mMessageParams.at(1), textParam("date", "2025-03-08"));
    EXPECT_EQ(selection.mDiagnostics.at(2).mMessageParams.at(0), textParam("currency", "USD"));
    EXPECT_EQ(selection.mDiagnostics.at(2).mMessageParams.at(1), textParam("date", "2025-03-08"));
}

TEST(OfficialRateSelectionTest, RubAfterItsLastRateDayIsMissing) {
    OfficialRateTable table;
    addFile(table, 2022);
    addRate(table, "RUB", "2022-03-01", 12'000'000'000); // 1 EUR = 120 RUB
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::RUB,
         .mTaxDate = date("2022-03-07"),
         .mUse = RateUse::IncomeGross,
         .mReport = ReportType::Dividends,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    // 2022-03-07 is 6 days after the last rate, beyond the 4 days.
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateNoRateOnDate);
}

TEST(OfficialRateSelectionTest, SaturdayTakesTheFridayRateWithAWarning) {
    OfficialRateTable table;
    addFile(table, 2025);
    addRate(table, "USD", "2025-03-07", 108'570'000); // Friday, 1 EUR = 1.0857 USD
    addRate(table, "USD", "2025-03-10", 108'000'000); // Monday
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-08"),
         .mUse = RateUse::IncomeGross,
         .mReport = ReportType::Dividends,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    const auto* rate = selection.find(Currency::USD, date("2025-03-08"));

    ASSERT_NE(rate, nullptr);
    EXPECT_EQ(rate->mValue, 108'570'000);
    EXPECT_EQ(rate->mObservationDate, date("2025-03-07"));
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);

    const auto& warning = selection.mDiagnostics.at(0);

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, ProcessingDiagnosticCode::OfficialRateFromNearbyDate);
    EXPECT_EQ(warning.mMessageKey, MessageKey::OfficialRateFromNearbyDate);
    EXPECT_EQ(warning.mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "USD"),
                                         textParam("date", "2025-03-08"),
                                         textParam("rateDate", "2025-03-07")}));
    EXPECT_EQ(warning.mContext.mTaxDate, date("2025-03-08"));
    EXPECT_EQ(warning.mContext.mAffectedReports, (std::vector<ReportType>{ReportType::Dividends}));

    // 10.0000 USD / 1.0857 = 9.2106475... EUR, rounded to 9.2106 EUR
    EXPECT_EQ(requireValue(convertOfficialMoney(100'000, *rate)), 92'106);
}

TEST(OfficialRateSelectionTest, SundayTakesTheFridayRateToo) {
    OfficialRateTable table;
    addFile(table, 2025);
    addRate(table, "USD", "2025-03-07", 108'570'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-09"),
         .mUse = RateUse::IncomeGross,
         .mReport = ReportType::Dividends,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    const auto* rate = selection.find(Currency::USD, date("2025-03-09"));

    ASSERT_NE(rate, nullptr);
    EXPECT_EQ(rate->mObservationDate, date("2025-03-07"));
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams.at(2),
              textParam("rateDate", "2025-03-07"));
}

TEST(OfficialRateSelectionTest, EasterMondayTakesTheThursdayRateFourDaysBack) {
    OfficialRateTable table;
    addFile(table, 2025);
    // Good Friday 2025-04-18 and Easter Monday 2025-04-21 have no ECB rate.
    addRate(table, "USD", "2025-04-17", 113'000'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-04-21"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    const auto* rate = selection.find(Currency::USD, date("2025-04-21"));

    ASSERT_NE(rate, nullptr);
    EXPECT_EQ(rate->mObservationDate, date("2025-04-17"));
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams.at(2),
              textParam("rateDate", "2025-04-17"));
}

TEST(OfficialRateSelectionTest, NewYearsDayTakesThePreviousYearsLastRateWhenBothFilesAreLoaded) {
    OfficialRateTable table;
    addFile(table, 2024);
    addFile(table, 2025);
    addRate(table, "USD", "2024-12-31", 103'890'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-01-01"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    const auto* rate = selection.find(Currency::USD, date("2025-01-01"));

    ASSERT_NE(rate, nullptr);
    EXPECT_EQ(rate->mObservationDate, date("2024-12-31"));
    EXPECT_EQ(rate->mValue, 103'890'000);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams.at(2),
              textParam("rateDate", "2024-12-31"));
}

TEST(OfficialRateSelectionTest, NewYearsDayNeedsThePreviousYearsFile) {
    OfficialRateTable table;
    addFile(table, 2025);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-01-01"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_EQ(selection.find(Currency::USD, date("2025-01-01")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey, MessageKey::MissingExchangeRateNoRateFile);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams.at(2), numberParam("year", 2024));
}

TEST(OfficialRateSelectionTest, AnEarlierRateIsNotUsedWhenTheFileOfTheTaxYearIsNotLoaded) {
    OfficialRateTable table;
    addFile(table, 2024);
    addRate(table, "USD", "2024-12-31", 103'890'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-01-02"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    // 2025-01-02 is a rate day; its own file is missing, so 2024-12-31 must not stand in.
    EXPECT_EQ(selection.find(Currency::USD, date("2025-01-02")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey, MessageKey::MissingExchangeRateNoRateFile);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams.at(2), numberParam("year", 2025));
}

TEST(OfficialRateSelectionTest, TwoEventsOnOneSaturdayGiveOneWarningWithBothSources) {
    OfficialRateTable table;
    addFile(table, 2025);
    addRate(table, "USD", "2025-03-07", 108'570'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-08"),
         .mUse = RateUse::IncomeGross,
         .mReport = ReportType::Dividends,
         .mSources = {source(2, "a")}},
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-08"),
         .mUse = RateUse::ForeignTax,
         .mReport = ReportType::Dividends,
         .mSources = {source(2, "a"), source(4, "c")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    // The same source twice is listed once.
    EXPECT_EQ(selection.mDiagnostics.at(0).mContext.mSources,
              (std::vector<SourceReference>{source(2, "a"), source(4, "c")}));
}

TEST(OfficialRateSelectionTest, AStoppedCurrencyOnAPublicationDayTakesNoEarlierRate) {
    OfficialRateTable table;
    addFile(table, 2022);
    addRate(table, "RUB", "2022-03-01", 12'000'000'000); // 1 EUR = 120 RUB, its last rate
    addRate(table, "USD", "2022-03-02", 110'000'000);    // the ECB still published on this day
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::RUB,
         .mTaxDate = date("2022-03-02"),
         .mUse = RateUse::IncomeGross,
         .mReport = ReportType::Dividends,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_EQ(selection.find(Currency::RUB, date("2022-03-02")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateNoRateOnDate);
    EXPECT_EQ(
        selection.mDiagnostics.at(0).mMessageParams,
        (std::vector<MessageParam>{textParam("currency", "RUB"), textParam("date", "2022-03-02")}));
}

TEST(OfficialRateSelectionTest, AStoppedCurrencyOnASaturdayStopsAtThePublicationDayBefore) {
    OfficialRateTable table;
    addFile(table, 2022);
    addRate(table, "RUB", "2022-03-01", 12'000'000'000); // Tuesday, its last rate
    addRate(table, "USD", "2022-03-04", 110'000'000);    // Friday, published without RUB
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::RUB,
         .mTaxDate = date("2022-03-05"),
         .mUse = RateUse::IncomeGross,
         .mReport = ReportType::Dividends,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    // 2022-03-05 is a Saturday. Friday had rates but none for RUB, so Tuesday is not taken.
    EXPECT_EQ(selection.find(Currency::RUB, date("2022-03-05")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateNoRateOnDate);
}

TEST(OfficialRateSelectionTest, ADateAfterTheEndOfAPartialYearFileIsNotTheEcbsFault) {
    OfficialRateTable table;
    addFile(table, 2025, "2025-01-03");
    addRate(table, "USD", "2025-01-03", 126'250'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-01-06"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    // 2025-01-06 is a Monday. The file ends on Friday 2025-01-03, so no rate is taken.
    EXPECT_EQ(selection.find(Currency::USD, date("2025-01-06")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateRateFileEndsEarly);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "USD"),
                                         textParam("date", "2025-01-06"),
                                         textParam("lastDate", "2025-01-03")}));
}

TEST(OfficialRateSelectionTest, AFileEndingWithinFourDaysOfNewYearsEveIsAWholeYear) {
    OfficialRateTable table;
    addFile(table, 2023, "2023-12-29"); // the last ECB day of 2023; 2023-12-31 is a Sunday
    addRate(table, "USD", "2023-12-29", 110'500'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2023-12-31"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    const auto* rate = selection.find(Currency::USD, date("2023-12-31"));

    ASSERT_NE(rate, nullptr);
    EXPECT_EQ(rate->mObservationDate, date("2023-12-29"));
    ASSERT_EQ(selection.mDiagnostics.size(), 2U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey, MessageKey::OfficialRateFromNearbyDate);

    // The file ends two days before the year does; the need after its end is told so.
    EXPECT_EQ(selection.mDiagnostics.at(1).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(selection.mDiagnostics.at(1).mCode,
              ProcessingDiagnosticCode::OfficialRateFileIncomplete);
    EXPECT_EQ(selection.mDiagnostics.at(1).mMessageKey,
              MessageKey::OfficialRateFileIncompleteEndsEarly);
    EXPECT_EQ(selection.mDiagnostics.at(1).mMessageParams,
              (std::vector<MessageParam>{numberParam("year", 2023),
                                         textParam("lastDate", "2023-12-29")}));
    EXPECT_EQ(selection.mDiagnostics.at(1).mContext.mSources,
              (std::vector<SourceReference>{source(2, "a")}));
}

TEST(OfficialRateSelectionTest, AFileEndingEarlyIsNotMentionedWhenNoNeedIsAfterItsEnd) {
    OfficialRateTable table;
    addFile(table, 2023, "2023-12-29");
    addRate(table, "USD", "2023-12-28", 110'400'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2023-12-28"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    ASSERT_NE(selection.find(Currency::USD, date("2023-12-28")), nullptr);
    EXPECT_TRUE(selection.mDiagnostics.empty());
}

TEST(OfficialRateSelectionTest, AFileThatStartsLateInTheYearGetsAWarning) {
    OfficialRateTable table;
    addFile(table, 2025, "", "2025-03-03"); // 2025-03-03 is a Monday
    addRate(table, "USD", "2025-03-04", 108'000'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-03-04"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    // The rate itself is found; only the file is flagged.
    ASSERT_NE(selection.find(Currency::USD, date("2025-03-04")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(selection.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::OfficialRateFileIncomplete);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::OfficialRateFileIncompleteStartsLate);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{numberParam("year", 2025),
                                         textParam("firstDate", "2025-03-03")}));
    EXPECT_EQ(selection.mDiagnostics.at(0).mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
}

TEST(OfficialRateSelectionTest, AFileStartingOnTheFirstWorkingDaysOfTheYearGetsNoWarning) {
    OfficialRateTable table;
    addFile(table, 2021, "", "2021-01-04"); // 2021-01-01 to 2021-01-03 were a holiday and a weekend
    addRate(table, "USD", "2021-01-05", 122'000'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2021-01-05"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    ASSERT_NE(selection.find(Currency::USD, date("2021-01-05")), nullptr);
    EXPECT_TRUE(selection.mDiagnostics.empty());
}

TEST(OfficialRateSelectionTest, FiveDaysBeforeNewYearsEveIsAnEarlyEnd) {
    OfficialRateTable table;
    addFile(table, 2023, "2023-12-26"); // 31 December minus 5 days
    addRate(table, "USD", "2023-12-26", 110'500'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2023-12-31"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_EQ(selection.find(Currency::USD, date("2023-12-31")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateRateFileEndsEarly);
}

TEST(OfficialRateSelectionTest, NewYearsDayNeedsThePreviousYearsFileToReachItsEnd) {
    OfficialRateTable table;
    addFile(table, 2024, "2024-12-20");
    addFile(table, 2025);
    addRate(table, "USD", "2024-12-20", 104'000'000);
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::USD,
         .mTaxDate = date("2025-01-01"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    EXPECT_EQ(selection.find(Currency::USD, date("2025-01-01")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateRateFileEndsEarly);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "USD"),
                                         textParam("date", "2025-01-01"),
                                         textParam("lastDate", "2024-12-20")}));
}

TEST(OfficialRateSelectionTest, HrkAfterTheEuroDateIsReplacedByEurEvenWithoutTheRateFile) {
    OfficialRateTable table;
    const std::vector<RateNeed> needs{
        {.mCurrency = Currency::HRK,
         .mTaxDate = date("2023-01-02"),
         .mUse = RateUse::SalePrice,
         .mReport = ReportType::CapitalGains,
         .mSources = {source(2, "a")}},
    };

    const auto selection = selectOfficialRates(needs, table);

    // 2023-01-02 is after the HRK euro date (2023-01-01); no rate file is loaded at all.
    EXPECT_EQ(selection.find(Currency::HRK, date("2023-01-02")), nullptr);
    ASSERT_EQ(selection.mDiagnostics.size(), 1U);
    EXPECT_EQ(selection.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageKey,
              MessageKey::MissingExchangeRateReplacedByEur);
    EXPECT_EQ(selection.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "HRK"),
                                         textParam("date", "2023-01-02"),
                                         textParam("euroDate", "2023-01-01")}));
}

} // namespace
