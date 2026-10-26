#include "cli/report_command.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
using taxbroker::cli::kExitBlocked;
using taxbroker::cli::kExitFailure;
using taxbroker::cli::kExitSuccess;
using taxbroker::cli::runCli;

struct CliRun {
    int mExitCode{};
    std::string mOut;
    std::string mErr;
};

/// A temporary folder for the input files of one test, with `out` as the output folder.
class TemporaryFolder {
  public:
    TemporaryFolder() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_report_command_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create the synthetic report-command directory"};
        }
    }

    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;

    ~TemporaryFolder() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    /// Writes the Trade Republic header and exactly the rows supplied by the test.
    [[nodiscard]] std::string writeCsv(std::string_view aName,
                                       std::initializer_list<std::string_view> aRows) const {
        std::string text =
            "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,"
            "amount,fee,tax,currency,original_amount,original_currency,fx_rate,description,"
            "transaction_id,counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

        for (const auto row : aRows)
        {
            text += std::string{row} + '\n';
        }

        return writeText(aName, text);
    }

    [[nodiscard]] std::string writeText(std::string_view aName, std::string_view aText) const {
        const auto path = mDirectory / aName;
        std::ofstream output{path, std::ios::binary};

        output << aText;
        output.close();

        if (!output)
        {
            throw std::runtime_error{"Unable to write a synthetic report-command file"};
        }

        return path.string();
    }

    [[nodiscard]] std::string path(std::string_view aName) const {
        return (mDirectory / aName).string();
    }

    [[nodiscard]] std::string outputDirectory() const {
        return path("out");
    }

    [[nodiscard]] bool hasOutput(std::string_view aName) const {
        return std::filesystem::exists(mDirectory / "out" / aName);
    }

    [[nodiscard]] std::string readOutput(std::string_view aName) const {
        std::ifstream input{mDirectory / "out" / aName, std::ios::binary};

        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

  private:
    std::filesystem::path mDirectory;
};

CliRun run(const std::vector<std::string>& aArguments) {
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = runCli(aArguments, out, err);

    return CliRun{exitCode, out.str(), err.str()};
}

bool contains(const std::string& aText, std::string_view aPart) {
    return aText.find(aPart) != std::string::npos;
}

constexpr std::string_view kTaxpayer = R"({"taxNumber": "12345678", "isResident": true})";

constexpr std::string_view kGeneralHelp =
    R"(taxbroker: turns broker CSV exports into Slovenian FURS tax forms (XML for eDavki).

Usage:
  taxbroker report --year YYYY --taxpayer FILE [--self-report] --out DIR CSV [CSV ...]
  taxbroker report --help
  taxbroker --help

Commands:
  report    Write the Doh-KDVP form (capital gains) for one year.

Run 'taxbroker report --help' for the details.
)";

constexpr std::string_view kUsageHint = "Run 'taxbroker report --help' for the usage.\n";

// Commands and help

TEST(ReportCommandTest, NoArgumentsWriteTheGeneralHelpToStderrAndFail) {
    const auto result = run({});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, kGeneralHelp);
}

TEST(ReportCommandTest, LongHelpOptionWritesTheGeneralHelp) {
    const auto result = run({"--help"});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_EQ(result.mOut, kGeneralHelp);
}

TEST(ReportCommandTest, ShortHelpOptionWritesTheGeneralHelp) {
    const auto result = run({"-h"});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_EQ(result.mOut, kGeneralHelp);
}

TEST(ReportCommandTest, HelpCommandWritesTheGeneralHelp) {
    const auto result = run({"help"});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_EQ(result.mOut, kGeneralHelp);
}

TEST(ReportCommandTest, CommandNamesAreCaseSensitive) {
    const auto result = run({"Report"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr,
              "taxbroker: unknown command Report.\nRun 'taxbroker --help' for the commands.\n");
}

TEST(ReportCommandTest, UnknownCommandWithFoldersPrintsOnlyItsLastPart) {
    const auto result = run({"reports/2024"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: unknown command 2024.\nRun 'taxbroker --help' for the commands.\n");
}

TEST(ReportCommandTest, ReportHelpStartsWithTheUsageAndEndsWithTheExample) {
    const auto result = run({"report", "--help"});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_EQ(result.mOut.rfind("Usage:\n"
                                "  taxbroker report --year YYYY --taxpayer FILE [--self-report] "
                                "--out DIR CSV [CSV ...]\n",
                                0),
              0U);
    EXPECT_TRUE(contains(result.mOut,
                         "Example:\n"
                         "  taxbroker report --year 2025 --taxpayer taxpayer.json --out out "
                         "export.csv\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  2  The command could not run: a wrong option, a missing file, an "
                         "invalid taxpayer\n"));
}

TEST(ReportCommandTest, ShortReportHelpOptionWritesTheReportHelp) {
    const auto result = run({"report", "-h"});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_TRUE(contains(result.mOut, "  -h, --help         Show this help.\n"));
}

TEST(ReportCommandTest, HelpAfterAnInvalidYearStillWritesTheReportHelp) {
    // The year is checked after all arguments are read, and help stops the reading.
    const auto result = run({"report", "--year", "1999", "--help"});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_TRUE(contains(result.mOut, "  -h, --help         Show this help.\n"));
}

TEST(ReportCommandTest, UnknownOptionBeforeHelpIsAnError) {
    // The arguments are read in order; the unknown option comes first.
    const auto result = run({"report", "--yaer", "--help"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: Unknown option --yaer.\n" + std::string{kUsageHint});
}

// Options

TEST(ReportCommandTest, UnknownShortOptionFails) {
    const auto result = run({"report", "-y", "2024"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: Unknown option -y.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, YearWithAnEqualsSignPrintsOnlyTheOptionName) {
    const auto result = run({"report", "--year=2024"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: Unknown option --year=...\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, TaxpayerWithoutAValueFails) {
    const auto result = run({"report", "--year", "2024", "--taxpayer"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: --taxpayer needs a value.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, OutputWithoutAValueFails) {
    const auto result = run({"report", "--year", "2024", "--out"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: --out needs a value.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, OptionAfterAnOptionIsTakenAsItsValue) {
    // "--out" is the value of --year, so the year is invalid.
    const auto result = run({"report", "--year", "--out", "out"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, YearGivenTwiceFails) {
    const auto result = run({"report", "--year", "2024", "--year", "2024"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: --year is given twice.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, TaxpayerGivenTwiceFails) {
    const auto result = run({"report", "--taxpayer", "a.json", "--taxpayer", "b.json"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: --taxpayer is given twice.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, OutputGivenTwiceFails) {
    const auto result = run({"report", "--out", "a", "--out", "b"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: --out is given twice.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, SelfReportGivenTwiceFails) {
    const auto result = run({"report", "--self-report", "--year", "2024", "--self-report"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: --self-report is given twice.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, NoOptionsReportTheMissingYearFirst) {
    const auto result = run({"report"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: --year is missing.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, EmptyYearFails) {
    const auto result = run({"report", "--year", ""});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, YearWithAPlusSignFails) {
    const auto result = run({"report", "--year", "+2024"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, YearWithALeadingSpaceFails) {
    const auto result = run({"report", "--year", " 2024"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, YearWithLettersAfterTheDigitsFails) {
    const auto result = run({"report", "--year", "2024x"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, NegativeYearFails) {
    const auto result = run({"report", "--year", "-2024"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, Year1999Fails) {
    const auto result = run({"report", "--year", "1999"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, Year10000Fails) {
    const auto result = run({"report", "--year", "10000"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, YearTooLargeForAnIntegerFails) {
    // 2^31 = 2147483648 does not fit in an int.
    const auto result = run({"report", "--year", "2147483648"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, Year2000IsAccepted) {
    // The year passes, so the next check reports the missing taxpayer file.
    const auto result = run({"report", "--year", "2000"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: --taxpayer is missing.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, Year9999IsAccepted) {
    const auto result = run({"report", "--year", "9999"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: --taxpayer is missing.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, MissingOutputFails) {
    const auto result = run({"report", "--year", "2024", "--taxpayer", "taxpayer.json"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: --out is missing.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, NoCsvFails) {
    const auto result =
        run({"report", "--year", "2024", "--taxpayer", "taxpayer.json", "--out", "out"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: Give at least one Trade Republic CSV file.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, CsvPathMayComeBeforeTheOptions) {
    // All options are read, so the next check looks for the CSV file.
    const auto result = run(
        {"report", "missing.csv", "--year", "2024", "--taxpayer", "taxpayer.json", "--out", "out"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file missing.csv does not exist or is not a file.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, HelpAfterDoubleDashIsACsvPath) {
    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             "taxpayer.json",
                             "--out",
                             "out",
                             "--",
                             "--help"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file --help does not exist or is not a file.\n" +
                  std::string{kUsageHint});
}

// Input files

TEST(ReportCommandTest, FirstMissingCsvIsNamed) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history,
                             folder.path("first-missing.csv"),
                             folder.path("second-missing.csv")});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file first-missing.csv does not exist or is not a file.\n" +
                  std::string{kUsageHint});
    EXPECT_FALSE(std::filesystem::exists(folder.outputDirectory()));
}

TEST(ReportCommandTest, CsvIsCheckedBeforeTheTaxpayerFile) {
    const TemporaryFolder folder;

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             folder.path("taxpayer.json"),
                             "--out",
                             folder.outputDirectory(),
                             folder.path("history.csv")});

    // Neither file exists; the CSV is named.
    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file history.csv does not exist or is not a file.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, CsvThatIsAFolderFails) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    std::filesystem::create_directory(folder.path("exports"));

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             folder.path("exports")});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file exports does not exist or is not a file.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, MissingTaxpayerFileFails) {
    const TemporaryFolder folder;
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             folder.path("taxpayer.json"),
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The taxpayer file taxpayer.json does not exist or is not a file.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, TaxpayerFileThatIsAFolderFails) {
    const TemporaryFolder folder;
    const auto history = folder.writeCsv("history.csv", {});
    std::filesystem::create_directory(folder.path("taxpayer"));

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             folder.path("taxpayer"),
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The taxpayer file taxpayer does not exist or is not a file.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, OutputThatIsAFileFails) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});
    const auto notAFolder = folder.writeText("out", "a file, not a folder");

    const auto result =
        run({"report", "--year", "2024", "--taxpayer", taxpayer, "--out", notAFolder, history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --out must be a folder, not a file.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, TaxpayerFileThatIsNotJsonFails) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", "taxNumber = 12345678");
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr,
              "taxbroker: the taxpayer file taxpayer.json is not valid: The taxpayer file is not "
              "valid JSON.\n" +
                  std::string{kUsageHint});
    EXPECT_FALSE(std::filesystem::exists(folder.outputDirectory()));
}

// Summary and output files

TEST(ReportCommandTest, GeneratedYearWritesTheXmlAndTheSummary) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2024 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2024.xml

Investments with a sale in 2024: 1
  XX0000000001 Synthetic Share: listed, 1 sale, 10.0000 units

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 0

Warnings: 0
)");

    EXPECT_EQ(folder.readOutput("summary_2024.txt"), result.mOut);
    EXPECT_TRUE(contains(folder.readOutput("Doh-KDVP_2024.xml"), "<ISIN>XX0000000001</ISIN>"));
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml.tmp"));
    EXPECT_FALSE(folder.hasOutput("summary_2024.txt.tmp"));
}

TEST(ReportCommandTest, SelfReportIsNamedInTheSummaryTitle) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--self-report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_EQ(result.mOut.rfind("TaxBrokerReport: Doh-KDVP 2024 (self-report)\n", 0), 0U);
}

TEST(ReportCommandTest, EachInputFileIsListedWithItsSourceIdAndFileName) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto first = folder.writeCsv("first.csv", {});
    const auto second = folder.writeCsv("second.csv", {});
    const auto third = folder.writeCsv("third.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             first,
                             second,
                             third});

    // Only the file names, never the folder.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Input files: 3\n"
                         "  source-0001 first.csv\n"
                         "  source-0002 second.csv\n"
                         "  source-0003 third.csv\n\n"));
    EXPECT_FALSE(contains(result.mOut, folder.path("")));
}

TEST(ReportCommandTest, YearWithoutDataWritesOnlyTheSummary) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2024 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): no data, no sale in 2024 needs a Doh-KDVP form

Investments with a sale in 2024: 0

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 0

Warnings: 0
)");
    EXPECT_TRUE(folder.hasOutput("summary_2024.txt"));
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
}

TEST(ReportCommandTest, YearWithoutDataDeletesAnOldXml) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});
    std::filesystem::create_directory(folder.outputDirectory());
    (void)folder.writeText("out/Doh-KDVP_2024.xml", "an old file of an earlier run");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): no data, no sale in 2024 needs a Doh-KDVP "
                         "form\n"
                         "Deleted the old Doh-KDVP_2024.xml of an earlier run.\n\n"));
}

TEST(ReportCommandTest, BlockedYearWithoutAnOldXmlDeletesNothing) {
    // Buy 10 @ 30, sell 10 @ 20: a loss, so the year is blocked.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n\n"));
    EXPECT_FALSE(contains(result.mOut, "Deleted the old"));
    EXPECT_TRUE(folder.hasOutput("summary_2024.txt"));
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
}

TEST(ReportCommandTest, UnitsAreWrittenWithFourToEightDecimals) {
    // Three investments, each bought at 10 and sold at 20, so none is a loss.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share A,XX0000000001,1.5,10,-15,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-01-11T09:00:00Z,2024-01-11,DEFAULT,TRADING,BUY,STOCK,Synthetic Share B,XX0000000002,0.12345678,10,-1.23,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-01-12T09:00:00Z,2024-01-12,DEFAULT,TRADING,BUY,STOCK,Synthetic Share C,XX0000000003,2.12345,10,-21.23,,,EUR,,,,,synthetic-buy-3,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share A,XX0000000001,-1.5,20,30,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-03-11T09:00:00Z,2024-03-11,DEFAULT,TRADING,SELL,STOCK,Synthetic Share B,XX0000000002,-0.12345678,20,2.47,,,EUR,,,,,synthetic-sell-2,,,,)",
            R"(2024-03-12T09:00:00Z,2024-03-12,DEFAULT,TRADING,SELL,STOCK,Synthetic Share C,XX0000000003,-2.12345,20,42.47,,,EUR,,,,,synthetic-sell-3,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // 1.5 gets zeros up to 4 decimals; 0.12345678 keeps all 8; 2.12345 keeps its 5.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Investments with a sale in 2024: 3\n"
                         "  XX0000000001 Synthetic Share A: listed, 1 sale, 1.5000 units\n"
                         "  XX0000000002 Synthetic Share B: listed, 1 sale, 0.12345678 units\n"
                         "  XX0000000003 Synthetic Share C: listed, 1 sale, 2.12345 units\n"));
}

TEST(ReportCommandTest, TwoSalesOfOneInvestmentAreCountedTogether) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,30,120,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-05-10T09:00:00Z,2024-05-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,30,90,,,EUR,,,,,synthetic-sell-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // 4 + 3 = 7 units in two sale rows.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(
        contains(result.mOut, "  XX0000000001 Synthetic Share: listed, 2 sales, 7.0000 units\n"));
}

TEST(ReportCommandTest, InvestmentWithoutAnErrorIsShownAsNoProblemWhenAnotherIsBlocked) {
    // The share is fine; the loss of the fund (5 @ 50 bought, 5 @ 40 sold) blocks the report.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-01-11T09:00:00Z,2024-01-11,DEFAULT,TRADING,BUY,FUND,Synthetic Fund,XX0000000003,5,50,-250,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-04-10T09:00:00Z,2024-04-10,DEFAULT,TRADING,SELL,FUND,Synthetic Fund,XX0000000003,-5,40,200,,,EUR,,,,,synthetic-sell-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "Investments with a sale in 2024: 2\n"
                         "  XX0000000001 Synthetic Share: no problem, 1 sale, 10.0000 units\n"
                         "  XX0000000003 Synthetic Fund: blocked (loss_eligibility_unresolved)\n"
                         "    sale 2024-04-10, history.csv row 5\n"));
}

TEST(ReportCommandTest, LongHeldUnitsAreListedApart) {
    // Sale in 2025, so 15 years: the 2 units from 2005 are past the holding period, the unit from
    // 2020 is not.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2005-04-01T09:00:00Z,2005-04-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2020-04-01T09:00:00Z,2020-04-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,20,-40,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2025-06-02T09:00:00Z,2025-06-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,30,90,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Investments with a sale in 2025: 1\n"
                         "  XX0000000001 Synthetic Share: listed, 1 sale, 1.0000 units\n\n"
                         "Units past the holding period (tax free, not in the XML): 1\n"
                         "  XX0000000001 Synthetic Share: sale 2025-06-02, 2.0000 units\n\n"));
}

TEST(ReportCommandTest, InvestmentWithOnlyLongHeldUnitsIsNotInTheXml) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2005-04-01T09:00:00Z,2005-04-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-06-02T09:00:00Z,2025-06-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,30,60,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: only units past the holding period, not "
                         "in the XML\n"));
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
}

TEST(ReportCommandTest, PrivateMarketEventsAreListedWithAndWithoutAnIsin) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-05-02T09:00:00Z,2024-05-02,DEFAULT,CASH,PRIVATE_MARKET_BUY,PRIVATE_FUND,Synthetic Private Fund,XX0000000009,,,-100,,,EUR,,,,,synthetic-private-buy-1,,,,)",
            // symbol is blank.
            R"(2024-11-15T09:00:00Z,2024-11-15,DEFAULT,CASH,PRIVATE_MARKET_SELL,PRIVATE_FUND,Synthetic Other Fund,,,,150,,,EUR,,,,,synthetic-private-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Private-market events skipped (only a cash amount, no units): 2\n"
                         "  XX0000000009 Synthetic Private Fund: 2024-05-02, history.csv row 2\n"
                         "  (no ISIN) Synthetic Other Fund: 2024-11-15, history.csv row 3\n\n"));
}

TEST(ReportCommandTest, DiagnosticsOfOtherFormsShowTheirSeverity) {
    // One dividend error and one dividend warning: both are listed apart with their severity.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            // amount is "ten", not a number.
            R"(2024-04-01T09:00:00Z,2024-04-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,ten,,,EUR,,,,,synthetic-dividend-1,,,,)",
            // name is blank.
            R"(2024-05-01T09:00:00Z,2024-05-01,DEFAULT,CASH,DIVIDEND,STOCK,,XX0000000001,,,10,,,EUR,,,,,synthetic-dividend-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 0\n\nWarnings: 0\n\n"
                         "Not about Doh-KDVP (dividends, interest and skipped private-market "
                         "rows): 2\n"
                         "  diagnostic-0001 error invalid_value, history.csv row 2\n"));
    EXPECT_TRUE(
        contains(result.mOut, "  diagnostic-0002 warning missing_field, history.csv row 3\n"));
}

TEST(ReportCommandTest, DiagnosticWithSeveralSourcesNamesTheFirstAndCountsTheOthers) {
    // The same transaction ID with another price in the second and the third file.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto first = folder.writeCsv(
        "first.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
        });
    const auto second = folder.writeCsv(
        "second.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,11,-11,,,EUR,,,,,synthetic-buy-1,,,,)",
        });
    const auto third = folder.writeCsv(
        "third.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,12,-12,,,EUR,,,,,synthetic-buy-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             first,
                             second,
                             third});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 conflicting_duplicate, XX0000000001 Synthetic Share, "
                         "2024-01-10, first.csv row 2 (+2 more)\n"));
}

TEST(ReportCommandTest, FileLevelErrorNamesOnlyTheFile) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    // 3 of the 23 columns.
    const auto other = folder.writeText("other.csv", "date,type,shares\n2024-01-10,BUY,1\n");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             other});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut, "Errors: 1\n  diagnostic-0001 invalid_header, other.csv\n"));
}

TEST(ReportCommandTest, DiagnosticWithoutASourceNamesOnlyItsCode) {
    // postCode has 13 characters; the form's postNumber allows 12.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText(
        "taxpayer.json",
        R"({"taxNumber": "12345678", "isResident": true, "postCode": "1234567890123"})");
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut, "Errors: 1\n  diagnostic-0001 xml_value_out_of_range\n"));
}

TEST(ReportCommandTest, BlockedInvestmentNamesEachCodeOnceAndEachSale) {
    // STOCK in the purchase and FUND in the two sales: the class conflict and the unknown class.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,FUND,Synthetic Share,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-05-10T09:00:00Z,2024-05-10,DEFAULT,TRADING,SELL,FUND,Synthetic Share,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked "
                         "(instrument_asset_class_conflict, tax_classification_unknown)\n"
                         "    sale 2024-03-10, history.csv row 3\n"
                         "    sale 2024-05-10, history.csv row 4\n\n"));
}

TEST(ReportCommandTest, OutputFolderIsCreatedWithItsParentFolders) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory() + "/2024/report",
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(folder.hasOutput("2024/report/summary_2024.txt"));
}

TEST(ReportCommandTest, NewRunReplacesTheOldSummary) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});
    std::filesystem::create_directory(folder.outputDirectory());
    (void)folder.writeText("out/summary_2024.txt", "an old summary of an earlier run");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_EQ(folder.readOutput("summary_2024.txt"), result.mOut);
}

// Output failures (exit 2: no output file changed, nothing printed on stdout)

TEST(ReportCommandTest, OutputFolderInsideAFileCannotBeCreated) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run(
        {"report", "--year", "2024", "--taxpayer", taxpayer, "--out", history + "/out", history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: the output folder cannot be created.\n");
}

TEST(ReportCommandTest, FolderAtTheXmlPathOfAGeneratedYearFails) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    std::filesystem::create_directories(folder.outputDirectory() + "/Doh-KDVP_2024.xml");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: Doh-KDVP_2024.xml cannot be written.\n");
    EXPECT_FALSE(folder.hasOutput("summary_2024.txt"));
}

TEST(ReportCommandTest, FolderAtTheXmlPathOfAYearWithoutDataFails) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});
    std::filesystem::create_directories(folder.outputDirectory() + "/Doh-KDVP_2024.xml");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // Even an empty folder there is not deleted: this run did not make it.
    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: the old Doh-KDVP_2024.xml cannot be deleted.\n");
    EXPECT_TRUE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_FALSE(folder.hasOutput("summary_2024.txt"));
}

TEST(ReportCommandTest, FolderAtTheSummaryPathFails) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});
    std::filesystem::create_directories(folder.outputDirectory() + "/summary_2024.txt");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: summary_2024.txt cannot be written.\n");
}

TEST(ReportCommandTest, FolderAtTheTemporaryXmlPathKeepsTheOldFiles) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    std::filesystem::create_directory(folder.outputDirectory());
    (void)folder.writeText("out/Doh-KDVP_2024.xml", "an old file of an earlier run");
    (void)folder.writeText("out/summary_2024.txt", "an old summary of an earlier run");
    std::filesystem::create_directory(folder.outputDirectory() + "/Doh-KDVP_2024.xml.tmp");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: Doh-KDVP_2024.xml cannot be written.\n");

    EXPECT_EQ(folder.readOutput("Doh-KDVP_2024.xml"), "an old file of an earlier run");
    EXPECT_EQ(folder.readOutput("summary_2024.txt"), "an old summary of an earlier run");
    EXPECT_FALSE(folder.hasOutput("summary_2024.txt.tmp"));
    // The folder in the way was not made by the run, so it stays.
    EXPECT_TRUE(folder.hasOutput("Doh-KDVP_2024.xml.tmp"));
}

TEST(ReportCommandTest, FolderAtTheTemporarySummaryPathDeletesTheNewTemporaryXml) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    std::filesystem::create_directory(folder.outputDirectory());
    std::filesystem::create_directory(folder.outputDirectory() + "/summary_2024.txt.tmp");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // The XML was written under its temporary name before the summary failed.
    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: summary_2024.txt cannot be written.\n");
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml.tmp"));
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
}

TEST(ReportCommandTest, FolderAtTheTemporarySummaryPathKeepsTheOldXmlOfAYearWithoutData) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});
    std::filesystem::create_directory(folder.outputDirectory());
    (void)folder.writeText("out/Doh-KDVP_2024.xml", "an old file of an earlier run");
    std::filesystem::create_directory(folder.outputDirectory() + "/summary_2024.txt.tmp");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // A year without data deletes the old XML, but not when the run fails with exit 2.
    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: summary_2024.txt cannot be written.\n");
    EXPECT_EQ(folder.readOutput("Doh-KDVP_2024.xml"), "an old file of an earlier run");
}

} // namespace
