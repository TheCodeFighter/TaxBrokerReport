#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/api/support_report_json.hpp"
#include "taxbroker/application_request.hpp"
#include "taxbroker/application_run.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {
using namespace taxbroker;

using Json = nlohmann::json;

// All rows are synthetic. The header is the one of the Trade Republic export.
constexpr std::string_view kTradeRepublicHeader =
    "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,"
    "currency,original_amount,original_currency,fx_rate,description,transaction_id,"
    "counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

/// The build as a test fixes it, so the golden file does not depend on the machine.
api::SupportReportApp fixedApp() {
    return {.mVersion = "2.0.0", .mBuild = "release", .mPlatform = "linux x86_64"};
}

TaxpayerMetadata syntheticTaxpayer() {
    return {.mTaxNumber = "12345679",
            .mName = "Synthetic Taxpayer",
            .mAddress = "Synthetic Street 1",
            .mCity = "Synthetic City",
            .mPostCode = "1000",
            .mPostName = "Synthetic City",
            .mBirthDate = *parseCalendarDate("1990-01-31"),
            .mIsResident = true,
            .mEmail = "taxpayer@example.com",
            .mPhoneNumber = "+38600000000"};
}

/// A request for 2024 with one Trade Republic file of the given name and content.
ApplicationRequest requestWithFile(std::string aFileName, std::string aRows) {
    ApplicationRequest request{
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
        .mTaxpayer = syntheticTaxpayer(),
    };

    request.mBrokerFiles.push_back(
        {.mBroker = Broker::TradeRepublic,
         .mFile = {.mName = std::move(aFileName),
                   .mContent = std::string{kTradeRepublicHeader} + std::move(aRows)}});

    return request;
}

/// The support report of a request that starts a run: the facts are taken before the run takes
/// the request, as the CLI and the server do.
std::string supportReportOf(ApplicationRequest aRequest) {
    const auto shape = api::supportReportRun(aRequest);
    auto started = ApplicationRun::start(std::move(aRequest));
    auto* run = std::get_if<ApplicationRun>(&started);

    if (run == nullptr)
    {
        throw std::runtime_error{"The run did not start"};
    }

    return api::serializeSupportReport(fixedApp(), shape, run->result());
}

/// A committed synthetic file under `tests/test_data`, read byte for byte.
std::string readTestData(std::string_view aName) {
    const auto path = std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" /
                      "support" / aName;
    std::ifstream input{path, std::ios::binary};

    if (!input.is_open())
    {
        throw std::runtime_error{"Unable to open the test file " + std::string{aName}};
    }

    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// Two purchases and one sale of XX0000000001: EUR 200 of basis, EUR 450 of proceeds.
constexpr std::string_view kFixtureRows =
    "2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic "
    "Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,\n"
    "2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic "
    "Share,XX0000000001,20,20,-400,,,EUR,,,,,synthetic-buy-2,,,,\n"
    "2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic "
    "Share,XX0000000001,-15,30,450,,,EUR,,,,,synthetic-sell,,,,\n";

TEST(SupportReportJsonTest, AGeneratedRunGivesTheGoldenFile) {
    const std::string text =
        supportReportOf(requestWithFile("export.csv", std::string{kFixtureRows}));

    EXPECT_EQ(text, readTestData("report_fixture_support.json"));
}

TEST(SupportReportJsonTest, AnIncompleteHistoryNamesCodeFieldAndIsin) {
    // A sale of XX0000000002 that was never bought: the history is incomplete.
    const std::string text = supportReportOf(
        requestWithFile("export.csv",
                        "2024-04-02T09:00:00Z,2024-04-02,DEFAULT,TRADING,SELL,STOCK,Other "
                        "Share,XX0000000002,-5,10,50,,,EUR,,,,,synthetic-sale-2,,,,\n"));
    const Json report = Json::parse(text);

    ASSERT_EQ(report["diagnostics"].size(), 1U);

    const Json& diagnostic = report["diagnostics"][0];

    EXPECT_EQ(diagnostic["code"], "incomplete_history");
    EXPECT_EQ(diagnostic["stage"], "historical_validation");
    EXPECT_EQ(diagnostic["isin"], "XX0000000002");
    EXPECT_EQ(diagnostic["affectedReports"], Json::parse(R"(["capitalGains"])"));
    // The file is source 1 and the sale is on row 2 (the header is row 1).
    EXPECT_EQ(diagnostic["sources"], Json::parse(R"([{"source": 1, "row": 2}])"));

    EXPECT_EQ(report["result"]["counts"]["errors"], 1);
    EXPECT_EQ(report["result"]["counts"]["byCode"], Json::parse(R"({"incomplete_history": 1})"));
    EXPECT_EQ(report["result"]["reports"]["capitalGains"]["status"], "needs_input");
    EXPECT_EQ(report["result"]["reports"]["capitalGains"]["requestTypes"],
              Json::parse(R"(["capital_gains_failure_resolution_required"])"));
    // The failure question lists the investment with its reason code; the ISIN is not a field of
    // the request itself.
    EXPECT_EQ(report["result"]["requests"][0]["investments"],
              Json::parse(R"([{"isin": "XX0000000002", "reasonCodes": ["incomplete_history"]}])"));
}

TEST(SupportReportJsonTest, TheReportHoldsNoPrivateFact) {
    // A purchase of 10 units at 123.45 (EUR 1234.50) with fee 1.23 and a transaction ID, then a
    // sale at a loss without a purchase near it, so the 30-day loss question is open.
    const std::string rows =
        "2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic "
        "Share,XX0000000001,10,123.45,-1234.50,-1.23,,EUR,,,,,secret-tx-7,,,,\n"
        "2024-06-10T09:00:00Z,2024-06-10,DEFAULT,TRADING,SELL,STOCK,Synthetic "
        "Share,XX0000000001,-10,100.00,1000.00,,,EUR,,,,,secret-tx-8,,,,\n";

    auto started = ApplicationRun::start(requestWithFile("Jane_Doe_export.csv", rows));
    auto* run = std::get_if<ApplicationRun>(&started);

    ASSERT_NE(run, nullptr);

    // The loss question is open, so the request holds the units and the prices.
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(api::requestTypeName(run->result().mRequests[0].mType),
              "loss_eligibility_confirmation_required");

    // Control: the application result does hold these facts, so the checks below can fail.
    const std::string application = api::serializeApplicationResultJson(run->result());

    EXPECT_NE(application.find("Synthetic Share"), std::string::npos);
    EXPECT_NE(application.find("secret-tx-8"), std::string::npos);
    EXPECT_NE(application.find("Jane_Doe_export.csv"), std::string::npos);

    const std::string text = api::serializeSupportReport(fixedApp(),
                                                         api::supportReportRun(run->request()),
                                                         run->result());

    for (const std::string_view fact : {"123.45",
                                        "1234.50",
                                        "1.23",
                                        "100.00",
                                        "secret-tx-7",
                                        "secret-tx-8",
                                        "Jane_Doe",
                                        "export.csv",
                                        "/tmp",
                                        "Synthetic Taxpayer",
                                        "Synthetic Street",
                                        "Synthetic Share",
                                        "12345679",
                                        "taxpayer@example.com"})
    {
        EXPECT_EQ(text.find(fact), std::string::npos) << fact;
    }
}

TEST(SupportReportJsonTest, TheSameRunGivesTheSameBytes) {
    const std::string first = supportReportOf(requestWithFile("a.csv", std::string{kFixtureRows}));
    const std::string second = supportReportOf(requestWithFile("a.csv", std::string{kFixtureRows}));

    EXPECT_EQ(first, second);
}

TEST(SupportReportJsonTest, ARunWithoutDiagnosticsHasOneLineFeedAtTheEnd) {
    const std::string text = supportReportOf(requestWithFile("a.csv", std::string{kFixtureRows}));

    EXPECT_TRUE(text.ends_with("}\n"));
    EXPECT_FALSE(text.ends_with("\n\n"));
}

TEST(SupportReportJsonTest, AnInputFaultHasNoResult) {
    ApplicationRequest request = requestWithFile("export.csv", std::string{kFixtureRows});

    // Not an ECB rate file: the columns after KEY and FREQ are missing.
    request.mEcbRateFiles.push_back({.mName = "rates.csv", .mContent = "KEY,FREQ\n"});

    const auto shape = api::supportReportRun(request);
    const auto started = ApplicationRun::start(std::move(request));
    const auto* fault = std::get_if<ApplicationInputFault>(&started);

    ASSERT_NE(fault, nullptr);

    const Json report = Json::parse(api::serializeSupportReport(fixedApp(), shape, *fault));

    EXPECT_FALSE(report.contains("result"));
    EXPECT_FALSE(report.contains("diagnostics"));
    EXPECT_EQ(report["run"]["ecbRateFiles"], 1);
    EXPECT_EQ(report["inputFault"]["kind"], "rate_file");
    EXPECT_FALSE(report["inputFault"]["messageKey"].get<std::string>().empty());
    EXPECT_EQ(report.dump().find("rates.csv"), std::string::npos);
}

TEST(SupportReportJsonTest, TheRunShapeCountsTheFilesPerBroker) {
    ApplicationRequest request{
        .mReportingYear = 2023,
        .mFilingType = FilingType::SelfReport,
        .mDeveloperMode = true,
    };

    request.mBrokerFiles.push_back({.mBroker = Broker::TradeRepublic, .mFile = {.mName = "a.csv"}});
    request.mBrokerFiles.push_back(
        {.mBroker = Broker::InteractiveBrokers, .mFile = {.mName = "b.csv"}});
    request.mBrokerFiles.push_back({.mBroker = Broker::TradeRepublic, .mFile = {.mName = "c.csv"}});
    request.mEcbRateFiles.push_back({.mName = "rates.csv"});
    request.mReferenceFiles.push_back({.mName = "payers.csv"});
    request.mReferenceFiles.push_back({.mName = "broker_payers.csv"});

    const auto shape = api::supportReportRun(request);

    EXPECT_EQ(shape.mYear, 2023);
    EXPECT_EQ(shape.mFilingType, FilingType::SelfReport);
    EXPECT_TRUE(shape.mDeveloperMode);
    ASSERT_EQ(shape.mBrokerFiles.size(), 2U);
    EXPECT_EQ(shape.mBrokerFiles[0].first, Broker::TradeRepublic);
    EXPECT_EQ(shape.mBrokerFiles[0].second, 2U); // a.csv and c.csv
    EXPECT_EQ(shape.mBrokerFiles[1].first, Broker::InteractiveBrokers);
    EXPECT_EQ(shape.mBrokerFiles[1].second, 1U);
    EXPECT_EQ(shape.mEcbRateFiles, 1U);
    EXPECT_EQ(shape.mReferenceFiles, 2U);
}

} // namespace
