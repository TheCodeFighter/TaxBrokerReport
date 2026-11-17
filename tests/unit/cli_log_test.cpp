#include "../support/log_capture.hpp"
#include "cli/report_command.hpp"
#include "taxbroker/logging.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
using taxbroker::LogCapture;
using taxbroker::cli::kExitFailure;
using taxbroker::cli::kExitSuccess;
using taxbroker::cli::logUnexpectedFailure;
using taxbroker::cli::runCli;

// The synthetic fixture of the CLI tests: two purchases and one sale of one share in 2024.
constexpr std::string_view kCsv =
    "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,"
    "currency,original_amount,original_currency,fx_rate,description,transaction_id,"
    "counterparty_name,counterparty_iban,payment_reference,mcc_code\n"
    "2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,"
    "-100,,,EUR,,,,,synthetic-buy-1,,,,\n"
    "2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,20,20,"
    "-400,,,EUR,,,,,synthetic-buy-2,,,,\n"
    "2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,"
    "30,450,,,EUR,,,,,synthetic-sell,,,,\n";

constexpr std::string_view kTaxpayer =
    R"({"taxNumber": "12345679", "isResident": true, "name": "Synthetic Taxpayer",
        "address": "Synthetic Street 1", "email": "taxpayer@example.com"})";

/// A temporary folder with the synthetic CSV and taxpayer file of one test.
class CliLogTest : public ::testing::Test {
  protected:
    void SetUp() override {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();

        mFolder = std::filesystem::temp_directory_path() / ("tbr_cli_log_" + std::to_string(token));
        std::filesystem::create_directories(mFolder);
        std::ofstream(mFolder / "synthetic_log.csv") << kCsv;
        std::ofstream(mFolder / "synthetic_taxpayer.json") << kTaxpayer;
    }

    void TearDown() override {
        std::error_code ignored;

        std::filesystem::remove_all(mFolder, ignored);
    }

    /// Runs `report` for 2024 without questions, with the extra arguments after the fixed ones.
    int report(std::vector<std::string> aExtra) const {
        std::vector<std::string> arguments{"report",
                                           "--year",
                                           "2024",
                                           "--taxpayer",
                                           (mFolder / "synthetic_taxpayer.json").string(),
                                           "--no-input",
                                           "--out",
                                           (mFolder / "out").string(),
                                           "--trade-republic",
                                           (mFolder / "synthetic_log.csv").string()};

        arguments.insert(arguments.begin() + 1, aExtra.begin(), aExtra.end());

        std::istringstream in;
        std::ostringstream out;
        std::ostringstream err;

        return runCli(arguments, in, out, err);
    }

    std::filesystem::path mFolder; ///< Always set by SetUp.
};

TEST_F(CliLogTest, ARunAtTheDefaultLevelWritesNoInfoLine) {
    LogCapture log;
    spdlog::default_logger()->set_level(taxbroker::defaultLogLevel());

    EXPECT_EQ(report({}), kExitSuccess);

    EXPECT_EQ(log.text(), "");
}

TEST_F(CliLogTest, ALogLevelOfInfoLogsTheStartTheStagesAndTheResult) {
    LogCapture log;

    EXPECT_EQ(report({"--log-level", "info"}), kExitSuccess);

    EXPECT_EQ(log.count("TaxBrokerReport 2.0.0 started: report ("), 1U);
    EXPECT_EQ(log.count("Stage rates: supplied files 0"), 1U);
    EXPECT_EQ(log.count("Stage reference: supplied files 0"), 1U);
    // Three Trade Republic rows are three events, from one source, without a problem.
    EXPECT_EQ(log.count("Stage parse and merge: sources 1, events 3, parse errors 0, "
                        "parse warnings 0, merge errors 0, merge warnings 0, conflicts 0 ("),
              1U);
    EXPECT_EQ(log.count("Stage year: instruments 1, capital events 3, events of the year 3"), 1U);
    EXPECT_EQ(log.count("Run started: year 2024, filing original, developer mode off, brokers "
                        "[Trade Republic x1], ecb rate files 0, reference files 0"),
              1U);
    EXPECT_EQ(log.count("Calculation done ("), 1U);
    EXPECT_EQ(log.count("Run result: revision=1 status=success capitalGains=generated"), 1U);
    EXPECT_EQ(log.count("Run details: requests [none]; errors [none]; warnings [none]"), 1U);
    EXPECT_EQ(log.count("Stage fifo:"), 0U); // a debug line
}

TEST_F(CliLogTest, ALogLevelOfDebugAddsTheCalculationStages) {
    LogCapture log;

    EXPECT_EQ(report({"--log-level", "debug"}), kExitSuccess);

    EXPECT_EQ(log.count("Stage classify: instruments 1, left out 0"), 1U);
    EXPECT_EQ(log.count("Stage fifo: pools 1, diagnostics 0"), 1U);
    EXPECT_EQ(log.count("Stage income: dividend rows 0, deposit interest rows 0, other interest "
                        "rows 0"),
              1U);
    EXPECT_EQ(log.count("Stage xml: forms written 1"), 1U); // only Doh-KDVP has data
}

TEST_F(CliLogTest, ALogLevelOfErrorHidesTheInfoLines) {
    LogCapture log;
    spdlog::default_logger()->set_level(spdlog::level::debug); // the option wins

    EXPECT_EQ(report({"--log-level", "error"}), kExitSuccess);

    EXPECT_EQ(log.count("Run started"), 0U);
    EXPECT_EQ(log.count("Stage fifo:"), 0U);
}

TEST_F(CliLogTest, AWrongLogLevelIsAUsageErrorWithoutALogLine) {
    LogCapture log;

    EXPECT_EQ(report({"--log-level", "loud"}), kExitFailure);

    EXPECT_EQ(log.text(), "");
}

TEST_F(CliLogTest, TheLogHoldsNoPrivateFact) {
    LogCapture log;

    EXPECT_EQ(report({"--log-level", "debug"}), kExitSuccess);

    // The ISIN, the name, the taxpayer's facts, the file names, the folder, the transaction IDs
    // and the amounts, as the CSV writes them (-100, -400) and at four decimals (EUR 100.0000,
    // 400.0000 and 450.0000).
    for (const std::string& fact : {std::string{"XX0000000001"},
                                    std::string{"Synthetic Share"},
                                    std::string{"Synthetic Taxpayer"},
                                    std::string{"Synthetic Street"},
                                    std::string{"12345679"},
                                    std::string{"taxpayer@example.com"},
                                    std::string{"synthetic_log.csv"},
                                    std::string{"synthetic_taxpayer.json"},
                                    mFolder.string(),
                                    std::string{"synthetic-buy-1"},
                                    std::string{"synthetic-sell"},
                                    std::string{"-100"},
                                    std::string{"-400"},
                                    std::string{"100.0000"},
                                    std::string{"400.0000"},
                                    std::string{"450.0000"}})
    {
        EXPECT_EQ(log.count(fact), 0U) << fact;
    }
}

TEST_F(CliLogTest, ABrokenRateFileLogsWhereTheRunStopped) {
    LogCapture log;

    // Not an ECB EXR csvdata file: one line of free text.
    std::ofstream(mFolder / "broken_rates.csv") << "this is not an ECB rate file\n";

    EXPECT_EQ(
        report({"--log-level", "info", "--ecb-rates", (mFolder / "broken_rates.csv").string()}),
        kExitFailure);

    EXPECT_EQ(log.count("Report stopped: input_fault (rate_file "), 1U);
    EXPECT_EQ(log.count("Run started"), 0U);
    EXPECT_EQ(log.count("broken_rates.csv"), 0U);
    EXPECT_EQ(log.count(mFolder.string()), 0U);
}

TEST_F(CliLogTest, AMissingInputFileLogsWhereTheRunStopped) {
    LogCapture log;

    // The Trade Republic file of the fixture is replaced by a name that was never written.
    std::vector<std::string> arguments{"report",
                                       "--year",
                                       "2024",
                                       "--taxpayer",
                                       (mFolder / "synthetic_taxpayer.json").string(),
                                       "--no-input",
                                       "--log-level",
                                       "info",
                                       "--out",
                                       (mFolder / "out").string(),
                                       "--trade-republic",
                                       (mFolder / "missing.csv").string()};
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;

    EXPECT_EQ(runCli(arguments, in, out, err), kExitFailure);

    EXPECT_EQ(log.count("Report stopped: input_files"), 1U);
    EXPECT_EQ(log.count("missing.csv"), 0U);
    EXPECT_EQ(log.count(mFolder.string()), 0U);
}

TEST_F(CliLogTest, AnInvalidTaxpayerFileLogsWhereTheRunStopped) {
    LogCapture log;

    // An empty object: none of the taxpayer's fields is there.
    std::ofstream(mFolder / "synthetic_taxpayer.json", std::ios::trunc) << "{}";

    EXPECT_EQ(report({"--log-level", "info"}), kExitFailure);

    EXPECT_EQ(log.count("Report stopped: taxpayer_file"), 1U);
    EXPECT_EQ(log.count("synthetic_taxpayer.json"), 0U);
    EXPECT_EQ(log.count("Run started"), 0U);
}

TEST(CliUnexpectedFailureTest, TheLineHoldsTheKindOnly) {
    LogCapture log;

    try
    { throw std::runtime_error("secret /home/x"); } catch (...)
    { logUnexpectedFailure(); }

    EXPECT_EQ(log.count("Report failed unexpectedly (std::runtime_error)"), 1U);
    EXPECT_EQ(log.count("secret"), 0U);
    EXPECT_EQ(log.count("/home/x"), 0U);
}

} // namespace
