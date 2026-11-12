#include "cli/decision_prompts.hpp"
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
using namespace taxbroker;

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

/// Runs the command with aInput as what the user types; without it the input ends at once.
CliRun run(const std::vector<std::string>& aArguments, std::string_view aInput = "") {
    std::istringstream in{std::string{aInput}};
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = runCli(aArguments, in, out, err);

    return CliRun{exitCode, out.str(), err.str()};
}

bool contains(const std::string& aText, std::string_view aPart) {
    return aText.find(aPart) != std::string::npos;
}

constexpr std::string_view kTaxpayer = R"({"taxNumber": "12345678", "isResident": true})";

constexpr std::string_view kGeneralHelp =
    R"(taxbroker: turns broker CSV exports into Slovenian FURS tax forms (XML for eDavki).

Usage:
  taxbroker report --year YYYY --taxpayer FILE [--self-report] [--no-input]
                   [--ecb-rates FILE...] [--reference-files FILE...] --out DIR
                   --trade-republic FILE...
  taxbroker report --help
  taxbroker --help

Commands:
  report    Write the tax forms Doh-KDVP, Doh-Div, Doh-DHO and Doh-Obr for one year.

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
                                "[--no-input]\n"
                                "                   [--ecb-rates FILE...] [--reference-files "
                                "FILE...] --out DIR\n"
                                "                   --trade-republic FILE...\n",
                                0),
              0U);
    EXPECT_TRUE(contains(result.mOut,
                         "Example:\n"
                         "  taxbroker report --year 2025 --taxpayer taxpayer.json --out out "
                         "--trade-republic export.csv\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  --trade-republic FILE...\n"
                         "                     One or more Trade Republic exports."));
    EXPECT_TRUE(contains(result.mOut,
                         "  --ecb-rates FILE...\n"
                         "                     ECB reference-rate files"));
    EXPECT_TRUE(contains(result.mOut,
                         "  --reference-files FILE...\n"
                         "                     A newer payers.csv, relief_statements.csv or "
                         "broker_payers.csv."));
    EXPECT_TRUE(contains(result.mOut, "The list ends at the next option"));
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

TEST(ReportCommandTest, EcbRatesWithoutAFileFails) {
    // The option is the last argument, so its list is empty.
    const auto result = run({"report", "--year", "2024", "--ecb-rates"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --ecb-rates needs at least one file.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, TradeRepublicFollowedByAnOptionFails) {
    // The next option ends the list before it has a file.
    const auto result = run({"report", "--trade-republic", "--year", "2024"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --trade-republic needs at least one file.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, ReferenceFilesWithoutAFileFails) {
    const auto result = run({"report", "--reference-files", "--out", "out"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --reference-files needs at least one file.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, AFileListTakesEveryArgumentUpToTheNextOption) {
    // Both lists have two files; the first missing file of the broker list is named, which
    // proves that "a.csv" and "b.csv" were both read as broker files and "r1.csv" and "r2.csv"
    // were not.
    const auto result = run({"report",
                             "--trade-republic",
                             "a.csv",
                             "b.csv",
                             "--ecb-rates",
                             "r1.csv",
                             "r2.csv",
                             "--year",
                             "2024",
                             "--taxpayer",
                             "taxpayer.json",
                             "--out",
                             "out"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file a.csv does not exist or is not a file.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, ASecondTradeRepublicOptionAddsToTheList) {
    // "a.csv" exists; "c.csv" comes after another option in a second --trade-republic and is the
    // first missing file, so the second list was added to the first.
    const TemporaryFolder folder;
    const auto first = folder.writeCsv("a.csv", {});

    const auto result = run({"report",
                             "--trade-republic",
                             first,
                             "--year",
                             "2024",
                             "--trade-republic",
                             folder.path("c.csv"),
                             "--taxpayer",
                             "taxpayer.json",
                             "--out",
                             "out"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file c.csv does not exist or is not a file.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, AFileWithoutAListOptionFails) {
    const auto result = run({"report", "--year", "2024", "a.csv"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: a.csv is not after a file option such as --trade-republic.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, AFileWithoutAListOptionPrintsOnlyItsLastPart) {
    const auto result = run({"report", "/home/someone/exports/a.csv"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: a.csv is not after a file option such as --trade-republic.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, AFileAfterAValueOptionIsNotInTheEarlierList) {
    // --out takes "out"; the list of --trade-republic ended at --out, so "b.csv" has no list.
    const auto result = run({"report", "--trade-republic", "a.csv", "--out", "out", "b.csv"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: b.csv is not after a file option such as --trade-republic.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, DoubleDashIsAnUnknownOption) {
    const auto result = run({"report", "--year", "2024", "--"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: Unknown option --.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, AFileNameThatStartsWithADashIsGivenWithADotSlash) {
    // "./-odd.csv" does not start with "-", so it is a file of the list.
    const auto result = run({"report",
                             "--trade-republic",
                             "./-odd.csv",
                             "--year",
                             "2024",
                             "--taxpayer",
                             "taxpayer.json",
                             "--out",
                             "out"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file -odd.csv does not exist or is not a file.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, AnEmptyArgumentWithoutAListOptionFails) {
    const auto result = run({"report", ""});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: An empty argument is not after a file option such as "
              "--trade-republic.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, OptionAfterAnOptionIsTakenAsItsValue) {
    // "--out" is the value of --year, so the year is invalid.
    const auto result = run({"report", "--year", "--out"});

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

TEST(ReportCommandTest, NoInputGivenTwiceFails) {
    const auto result = run({"report", "--no-input", "--year", "2024", "--no-input"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr, "taxbroker: --no-input is given twice.\n" + std::string{kUsageHint});
}

TEST(ReportCommandTest, NoInputMayComeAfterTheTradeRepublicList) {
    // --no-input ends the list and is read as an option, so the next check names the CSV file.
    const auto result = run({"report",
                             "--trade-republic",
                             "missing.csv",
                             "--no-input",
                             "--year",
                             "2024",
                             "--taxpayer",
                             "taxpayer.json",
                             "--out",
                             "out"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file missing.csv does not exist or is not a file.\n" +
                  std::string{kUsageHint});
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
              "taxbroker: Give at least one Trade Republic CSV file with --trade-republic.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, TheTradeRepublicListMayComeBeforeTheOtherOptions) {
    // All options are read, so the next check looks for the CSV file.
    const auto result = run({"report",
                             "--trade-republic",
                             "missing.csv",
                             "--year",
                             "2024",
                             "--taxpayer",
                             "taxpayer.json",
                             "--out",
                             "out"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file missing.csv does not exist or is not a file.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, HelpAfterAFileOptionIsStillTheHelp) {
    // "--help" is an option, so it ends the empty list and shows the help.
    const auto result = run({"report", "--trade-republic", "--help"});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_TRUE(contains(result.mOut, "  -h, --help         Show this help.\n"));
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
                             "--trade-republic",
                             history,
                             folder.path("first-missing.csv"),
                             folder.path("second-missing.csv")});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file first-missing.csv does not exist or is not a file.\n" +
                  std::string{kUsageHint});
    EXPECT_FALSE(std::filesystem::exists(folder.outputDirectory()));
}

TEST(ReportCommandTest, ACsvFileThatCannotBeReadIsNamedAndFailsTheRun) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("locked.csv", {});

    // The file exists and is a regular file, so only reading it fails.
    std::filesystem::permissions(history, std::filesystem::perms::none);

    if (std::ifstream{history})
    {
        GTEST_SKIP() << "The process can read a file without permissions (for example as root).";
    }

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: the CSV file locked.csv could not be read.\n" + std::string{kUsageHint});
    EXPECT_FALSE(std::filesystem::exists(folder.outputDirectory()));
}

TEST(ReportCommandTest, AnEcbRateFileThatCannotBeReadIsNamedAndFailsTheRun) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});
    const auto rates = folder.writeText("locked_rates.csv", "");

    std::filesystem::permissions(rates, std::filesystem::perms::none);

    if (std::ifstream{rates})
    {
        GTEST_SKIP() << "The process can read a file without permissions (for example as root).";
    }

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history,
                             "--ecb-rates",
                             rates});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: the ECB rate file locked_rates.csv could not be read.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, AReferenceFileThatCannotBeReadIsNamedAndFailsTheRun) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});
    const auto reference = folder.writeText("locked_payers.csv", "");

    std::filesystem::permissions(reference, std::filesystem::perms::none);

    if (std::ifstream{reference})
    {
        GTEST_SKIP() << "The process can read a file without permissions (for example as root).";
    }

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history,
                             "--reference-files",
                             reference});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: the reference file locked_payers.csv could not be read.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, ACsvFileOverTheSizeLimitIsNamedWithTheLimitAndFailsTheRun) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeText("big.csv", "");

    // 100 MB + 1 byte, as a sparse file: the size is checked before anything is read.
    std::filesystem::resize_file(history, 100U * 1024U * 1024U + 1U);

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: the CSV file big.csv cannot be used: The file is larger than 100 MB and "
              "was not read. Check that you chose the right file.\n" +
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
                             "--trade-republic",
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
                             "--trade-republic",
                             folder.path("exports")});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file exports does not exist or is not a file.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, FirstMissingEcbRateFileIsNamed) {
    // --ecb-rates takes a list; the first file of it that is missing is named.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});
    const auto rates2024 = folder.writeText("exr_2024.csv", "KEY\n");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--ecb-rates",
                             rates2024,
                             folder.path("exr_2025.csv"),
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The ECB rate file exr_2025.csv does not exist or is not a file.\n" +
                  std::string{kUsageHint});
    EXPECT_FALSE(std::filesystem::exists(folder.outputDirectory()));
}

TEST(ReportCommandTest, AnEcbRatePathWithoutAFileNameIsNamedByAPlaceholder) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    // The path ends in a separator, so it has no file name and no folder is printed.
    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--ecb-rates",
                             folder.path("rates") + "/",
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The ECB rate file <no file name> does not exist or is not a file.\n" +
                  std::string{kUsageHint});
}

TEST(ReportCommandTest, FirstMissingReferenceFileIsNamed) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});
    const auto payers = folder.writeText("payers.csv", "isin\n");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--reference-files",
                             payers,
                             folder.path("broker_payers.csv"),
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The reference file broker_payers.csv does not exist or is not a "
              "file.\n" +
                  std::string{kUsageHint});
    EXPECT_FALSE(std::filesystem::exists(folder.outputDirectory()));
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
                             "--trade-republic",
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
                             "--trade-republic",
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

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             notAFolder,
                             "--trade-republic",
                             history});

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
                             "--trade-republic",
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
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport 2024 (original return)
Input files: 1
  source-0001 history.csv
Reference data: built-in (ib-edavki 22220f5)
ECB rate files: built-in 2016 to 2025

Forms:
  Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2024.xml
  Dividends (Doh-Div): no data, no dividend to report in 2024
  Bank-deposit interest (Doh-DHO): no data, no bank-deposit interest to report in 2024
  Other interest (Doh-Obr): no data, no cash interest or bond coupon to report in 2024

Investments with a sale in 2024: 1
  XX0000000001 Synthetic Share: listed, 1 sale, 10.0000 units

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Doh-KDVP errors: 0

Doh-KDVP warnings: 0
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
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_EQ(result.mOut.rfind("TaxBrokerReport 2024 (self-report)\n", 0), 0U);
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
                             "--trade-republic",
                             first,
                             second,
                             third});

    // Only the file names, never the folder.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Input files: 3\n"
                         "  source-0001 first.csv\n"
                         "  source-0002 second.csv\n"
                         "  source-0003 third.csv\n"
                         "Reference data: built-in (ib-edavki 22220f5)\n"
                         "ECB rate files: built-in 2016 to 2025\n\n"));
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
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport 2024 (original return)
Input files: 1
  source-0001 history.csv
Reference data: built-in (ib-edavki 22220f5)
ECB rate files: built-in 2016 to 2025

Forms:
  Capital gains (Doh-KDVP): no data, no sale in 2024 needs a Doh-KDVP form
  Dividends (Doh-Div): no data, no dividend to report in 2024
  Bank-deposit interest (Doh-DHO): no data, no bank-deposit interest to report in 2024
  Other interest (Doh-Obr): no data, no cash interest or bond coupon to report in 2024

Investments with a sale in 2024: 0

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Doh-KDVP errors: 0

Doh-KDVP warnings: 0
)");
    EXPECT_TRUE(folder.hasOutput("summary_2024.txt"));
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
}

TEST(ReportCommandTest, YearWithoutDataKeepsAnOldXmlAndSaysSo) {
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
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_EQ(folder.readOutput("Doh-KDVP_2024.xml"), "an old file of an earlier run");
    EXPECT_TRUE(contains(result.mOut,
                         "  Capital gains (Doh-KDVP): no data, no sale in 2024 needs a Doh-KDVP "
                         "form\n"
                         "    Doh-KDVP_2024.xml in the output folder is from an earlier run, not "
                         "from this run.\n"
                         "  Dividends (Doh-Div): no data"));
}

TEST(ReportCommandTest, BlockedYearWithoutAnOldXmlSaysNothingAboutOne) {
    // Buy 1 @ 10 and 1 @ 40, sell 2 @ 30: +20 and -10. The sale empties the holding, so the loss
    // question is asked; the input ends without an answer, so the question blocks the year.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,40,-40,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-04-10T09:00:00Z,2024-04-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,30,60,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "  Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"
                         "  Dividends (Doh-Div): no data"));
    EXPECT_FALSE(contains(result.mOut, "from an earlier run"));
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
                             "--trade-republic",
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
                             "--trade-republic",
                             history});

    // 4 + 3 = 7 units in two sale rows.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(
        contains(result.mOut, "  XX0000000001 Synthetic Share: listed, 2 sales, 7.0000 units\n"));
}

TEST(ReportCommandTest, InvestmentWithoutAnErrorIsShownAsNoProblemWhenAnotherIsBlocked) {
    // The share is sold at a loss with no purchase within 30 days; the answer yes blocks the loss
    // of its 10 units (F10 false), which the list can say, so the share is fine. The fund is a
    // private fund, which Doh-KDVP does not cover here, so it blocks the report.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-01-11T09:00:00Z,2024-01-11,DEFAULT,TRADING,BUY,PRIVATE_FUND,Synthetic Private Fund,XX0000000003,5,10,-50,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-04-10T09:00:00Z,2024-04-10,DEFAULT,TRADING,SELL,PRIVATE_FUND,Synthetic Private Fund,XX0000000003,-5,40,200,,,EUR,,,,,synthetic-sell-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history},
                            "yes\n");

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(
        contains(result.mOut,
                 "Investments with a sale in 2024: 2\n"
                 "  XX0000000001 Synthetic Share: no problem, 1 sale, 10.0000 units, "
                 "10.0000 units with a loss blocked by the 30-day rule (F10 false)\n"
                 "  XX0000000003 Synthetic Private Fund: blocked (unsupported_tax_treatment)\n"
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
                             "--trade-republic",
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
                             "--trade-republic",
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
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Private-market events skipped (only a cash amount, no units): 2\n"
                         "  XX0000000009 Synthetic Private Fund: 2024-05-02, history.csv row 2\n"
                         "  (no ISIN) Synthetic Other Fund: 2024-11-15, history.csv row 3\n\n"));
}

TEST(ReportCommandTest, DividendDiagnosticsAreListedUnderDohDivAndBlockOnlyIt) {
    // One dividend error, one parser warning and one stage warning (the payer is not in the
    // reference files): all belong to Doh-Div. The rejected row blocks Doh-Div, and only it.
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
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "  Dividends (Doh-Div): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Doh-KDVP errors: 0\n\nDoh-KDVP warnings: 0\n\n"
                         "Doh-Div errors: 1\n"
                         "  diagnostic-0001 invalid_value, history.csv row 2\n"));
    EXPECT_TRUE(contains(result.mOut, "  diagnostic-0002 missing_field, history.csv row 3\n"));
    // The second dividend (row 3, 2024-05-01) is the only one that became a payment.
    EXPECT_TRUE(contains(result.mOut,
                         "  diagnostic-0003 dividend_payer_not_found, XX0000000001, "
                         "2024-05-01, history.csv row 3\n"));
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
                             "--trade-republic",
                             first,
                             second,
                             third});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "Doh-KDVP errors: 1\n"
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
                             "--trade-republic",
                             other});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(
        contains(result.mOut, "Doh-KDVP errors: 1\n  diagnostic-0001 invalid_header, other.csv\n"));
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
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(
        contains(result.mOut, "Doh-KDVP errors: 1\n  diagnostic-0001 xml_value_out_of_range\n"));
}

TEST(ReportCommandTest, BlockedInvestmentNamesEachCodeOnceAndEachSale) {
    // A private fund bought once (1 unit) and sold twice: the first sale needs more units than
    // are held (incomplete history) and the class has no capital treatment (unsupported).
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,PRIVATE_FUND,Synthetic Private Fund,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,PRIVATE_FUND,Synthetic Private Fund,XX0000000001,-2,30,60,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-05-10T09:00:00Z,2024-05-10,DEFAULT,TRADING,SELL,PRIVATE_FUND,Synthetic Private Fund,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Private Fund: blocked "
                         "(incomplete_history, unsupported_tax_treatment)\n"
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
                             "--trade-republic",
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
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_EQ(folder.readOutput("summary_2024.txt"), result.mOut);
}

// Excluded items

TEST(ReportCommandTest, ACryptoPurchaseIsListedAsATaxRuleExclusionWithoutAWarning) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,CRYPTO,Synthetic Coin,XX0000000005,1,50,-50,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history});

    // The share sale is written; the crypto purchase is only listed.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_FALSE(contains(folder.readOutput("Doh-KDVP_2024.xml"), "XX0000000005"));
    EXPECT_TRUE(contains(result.mOut,
                         "Excluded items (left out of the reports, not in the XML): 1\n"
                         "  XX0000000005 Synthetic Coin: crypto, tax rule: crypto is not part of "
                         "Doh-KDVP; check whether a separate crypto return applies to you\n"));
    EXPECT_TRUE(contains(result.mOut, "Doh-KDVP errors: 0\n\nDoh-KDVP warnings: 0\n"));
}

TEST(ReportCommandTest, ABondBoughtAndSoldInTheYearListsItsUnitsWithoutAMaturityDate) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-02-03T09:00:00Z,2024-02-03,DEFAULT,TRADING,BUY,BOND,Synthetic Bond,XS0000000001,400,1,-400,,,EUR,,,,,synthetic-bond-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,BOND,Synthetic Bond,XS0000000001,-250,1,250,,,EUR,,,,,synthetic-bond-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history});

    // 400 units bought and 250 sold, none matured: the line ends after the units, with no
    // maturity date. A bond sold in the year asks its type, and with no input Doh-Obr is blocked.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "Excluded items (left out of the reports, not in the XML): 1\n"
                         "  XS0000000001 Synthetic Bond: bond, tax rule: debt security, only its "
                         "interest is taxable; bought 400.0000, sold 250.0000, matured 0.0000 "
                         "units\n"));
    EXPECT_TRUE(contains(result.mOut, "Doh-KDVP errors: 0\n\nDoh-KDVP warnings: 0\n"));
}

TEST(ReportCommandTest, AMaturedBondListsItsMaturityDate) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-02-03T09:00:00Z,2025-02-03,DEFAULT,TRADING,BUY,BOND,Synthetic Bond,XS0000000001,1000,1,-1000,,,EUR,,,,,synthetic-bond-buy-1,,,,)",
            R"(2025-08-14T15:01:00.000Z,2025-08-14,DEFAULT,CORPORATE_ACTION,FINAL_MATURITY,BOND,Synthetic Bond,XS0000000001,-1000,,,,,,,,,FINAL_MATURITY XS0000000001,synthetic-bond-maturity-001,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history});

    // A bond matured in the year asks its type, and with no input Doh-Obr is blocked.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "Excluded items (left out of the reports, not in the XML): 1\n"
                         "  XS0000000001 Synthetic Bond: bond, tax rule: debt security, only its "
                         "interest is taxable; bought 1000.0000, sold 0.0000, matured 1000.0000 "
                         "units, maturity 2025-08-14\n"));
}

TEST(ReportCommandTest, AnUnknownClassAnsweredExcludeIsListedAsYourChoice) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,,Synthetic Mystery,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,,Synthetic Mystery,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history},
                            "exclude\n");

    // The class is unknown and the user chose to leave the investment out: no tax rule applies.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Excluded items (left out of the reports, not in the XML): 1\n"
                         "  XX0000000001 Synthetic Mystery: unknown class, your choice\n"));
}

// Output failures (exit 2: no output file changed, nothing printed on stdout)

TEST(ReportCommandTest, OutputFolderInsideAFileCannotBeCreated) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             history + "/out",
                             "--trade-republic",
                             history});

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
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: Doh-KDVP_2024.xml cannot be written.\n");
    EXPECT_FALSE(folder.hasOutput("summary_2024.txt"));
}

TEST(ReportCommandTest, FolderAtTheXmlPathOfAYearWithoutDataIsLeftAlone) {
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
                             "--trade-republic",
                             history});

    // A form without data writes nothing, so the folder is not touched, and it is not an old file
    // of an earlier run.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_FALSE(contains(result.mOut, "from an earlier run"));
    EXPECT_TRUE(std::filesystem::is_directory(folder.outputDirectory() + "/Doh-KDVP_2024.xml"));
    EXPECT_TRUE(folder.hasOutput("summary_2024.txt"));
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
                             "--trade-republic",
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
                             "--trade-republic",
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
                             "--trade-republic",
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
                             "--trade-republic",
                             history});

    // A year without data deletes the old XML, but not when the run fails with exit 2.
    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: summary_2024.txt cannot be written.\n");
    EXPECT_EQ(folder.readOutput("Doh-KDVP_2024.xml"), "an old file of an earlier run");
}

// Questions

TEST(ReportCommandTest, EveryQuestionWithQuestionDataIsAsked) {
    // A question without question data cannot be answered.
    const QuestionData noData{std::monostate{}};
    const QuestionData failure{FailureResolutionQuestion{
        .mInvestments = {{.mIsin = "XX0000000001", .mName = "Synthetic Share", .mReasonCodes = {}}},
    }};
    const QuestionData loss{
        LossEligibilityQuestion{.mIsin = "XX0000000001", .mName = "Synthetic Share"}};
    const QuestionData ratio{
        CorporateActionRatioQuestion{.mIsin = "XX0000000001", .mName = "Synthetic Share"}};

    EXPECT_FALSE(cli::canAsk(noData));
    EXPECT_TRUE(cli::canAsk(failure));
    EXPECT_TRUE(cli::canAsk(loss));
    EXPECT_TRUE(cli::canAsk(ratio));
}

TEST(ReportCommandTest, AnAnswerToAFailureQuestionCountsTheFailedInvestments) {
    const RecordedDecision exclude{
        .mId = "decision-0001",
        .mType = RequestType::CapitalGainsFailureResolutionRequired,
        .mData =
            FailureResolutionQuestion{
                .mInvestments =
                    {
                        {.mIsin = "XX0000000001", .mName = "Synthetic Share", .mReasonCodes = {}},
                        {.mIsin = "XX0000000002", .mName = "Synthetic Fund", .mReasonCodes = {}},
                    }},
        .mAnswer = FailureResolutionAnswer{.mChoice = FailureResolution::ExcludeFailedInstruments},
        .mFromEarlierRun = false,
        .mUsed = true,
    };
    const RecordedDecision fail{
        .mId = "decision-0002",
        .mType = RequestType::CapitalGainsFailureResolutionRequired,
        .mData =
            FailureResolutionQuestion{
                .mInvestments =
                    {
                        {.mIsin = "XX0000000001", .mName = "Synthetic Share", .mReasonCodes = {}},
                    }},
        .mAnswer = FailureResolutionAnswer{.mChoice = FailureResolution::FailReport},
        .mFromEarlierRun = false,
        .mUsed = true,
    };

    EXPECT_EQ(cli::answerLine(exclude), "2 failed investments: leave the failed investments out");
    EXPECT_EQ(cli::answerLine(fail), "1 failed investment: block the report");
}

TEST(ReportCommandTest, AnAnswerToAFailureQuestionWithBrokersCountsInvestmentsAndBrokers) {
    const RecordedDecision exclude{
        .mId = "decision-0001",
        .mType = RequestType::OtherInterestFailureResolutionRequired,
        .mData =
            FailureResolutionQuestion{
                .mInvestments =
                    {
                        {.mIsin = "XX0000000010", .mName = "Synthetic Bond", .mReasonCodes = {}},
                    },
                .mBrokers = {{.mBroker = Broker::TradeRepublic, .mReasonCodes = {}}}},
        .mAnswer = FailureResolutionAnswer{.mChoice = FailureResolution::ExcludeFailedInstruments},
        .mFromEarlierRun = false,
        .mUsed = true,
    };

    EXPECT_EQ(cli::answerLine(exclude), "2 failed items: leave the failed items out");
}

TEST(ReportCommandTest, AnAnswerToAQuestionWithoutDataNamesTheRequestType) {
    const RecordedDecision decision{
        .mId = "decision-0001",
        .mType = RequestType::ExchangeRatesRequired,
        .mData = std::monostate{},
        .mAnswer = CancelAnswer{},
        .mFromEarlierRun = false,
        .mUsed = true,
    };

    EXPECT_EQ(cli::answerLine(decision), "exchange_rates_required: cancelled");
}

// Replacing the output files

TEST(ReportCommandTest, AnOldFileThatCannotBeMovedAsideFailsTheRunAndKeepsTheOldFiles) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});
    std::filesystem::create_directory(folder.outputDirectory());
    (void)folder.writeText("out/summary_2024.txt", "an old summary of an earlier run");
    // The earlier summary is moved to summary_2024.txt.old first; a file cannot be renamed onto
    // a folder, so that step fails.
    std::filesystem::create_directory(folder.outputDirectory() + "/summary_2024.txt.old");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: the output files cannot be replaced.\n");
    EXPECT_EQ(folder.readOutput("summary_2024.txt"), "an old summary of an earlier run");
    EXPECT_FALSE(folder.hasOutput("summary_2024.txt.tmp"));
}

// ECB rate files named by an income form

/// An ECB file of 2026 that ends on 2026-11-10, far from 31 December.
constexpr std::string_view kEcbUsd2026UntilNovember =
    R"(KEY,FREQ,CURRENCY,CURRENCY_DENOM,EXR_TYPE,EXR_SUFFIX,TIME_PERIOD,OBS_VALUE,OBS_STATUS
EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2026-01-02,1.25,A
EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2026-03-02,1.25,A
EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2026-11-10,1.25,A
)";

TEST(ReportCommandTest, ADividendWithoutAnEcbFileOfItsYearNamesTheFileToLoad) {
    // The built-in rates end with 2025, and no file of 2026 is given.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2026-03-02T09:00:00Z,2026-03-02,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Dollar Share,XX0000000002,,,8.00,,,EUR,10.00,USD,0.800000,,synthetic-dividend-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2026",
                             "--taxpayer",
                             taxpayer,
                             "--no-input",
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-Div_2026.xml"));
    EXPECT_TRUE(contains(result.mOut, "Doh-Div errors: 1\n"));
    EXPECT_TRUE(contains(result.mOut, "missing_exchange_rate"));
    EXPECT_TRUE(contains(result.mOut, "  ECB rate files to load: 2026\n"));
    EXPECT_FALSE(contains(result.mOut, "ECB rate files to replace"));
}

TEST(ReportCommandTest, ADividendAfterTheLastDayOfAnEcbFileNamesTheFileToReplace) {
    // The file ends on 2026-11-10, so it cannot tell whether 2026-11-12 had a rate.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto rates = folder.writeText("rates.csv", kEcbUsd2026UntilNovember);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2026-11-12T09:00:00Z,2026-11-12,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Dollar Share,XX0000000002,,,8.00,,,EUR,10.00,USD,0.800000,,synthetic-dividend-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2026",
                             "--taxpayer",
                             taxpayer,
                             "--no-input",
                             "--out",
                             folder.outputDirectory(),
                             "--ecb-rates",
                             rates,
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-Div_2026.xml"));
    EXPECT_TRUE(contains(result.mOut, "Doh-Div errors: 1\n"));
    EXPECT_TRUE(contains(result.mOut, "  ECB rate files to replace (they end too early): 2026\n"));
    EXPECT_FALSE(contains(result.mOut, "ECB rate files to load"));
}

// What the user left out of an income form

TEST(ReportCommandTest, AnExcludedBrokerLeavesDohObrWrittenFromTheCoupons) {
    // The Trade Republic interest of 2023-02-01 is before 6 December 2023, so it is cash interest
    // of Doh-Obr, in a currency (HRK) that the euro replaced on 2023-01-01: no rate exists. The
    // user leaves the broker's cash interest out; the EUR coupon stays.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2023-02-01T10:00:00Z,2023-02-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,1.33,,,EUR,10.00,HRK,0.133000,Synthetic broker interest,synthetic-interest-1,,,,)",
            R"(2023-03-10T10:00:00Z,2023-03-10,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond B,XS0000000002,,,10.00,,,EUR,,,,Interest payment,synthetic-coupon-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2023",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history},
                            "exclude\n");

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "  Other interest (Doh-Obr): generated without 1 excluded broker, 1 row, "
                         "written to Doh-Obr_2023.xml\n"));
    EXPECT_TRUE(contains(result.mOut, "Excluded brokers (not in Doh-Obr): Trade Republic\n"));
    EXPECT_TRUE(
        contains(result.mOut,
                 "    Resolved by your answer: the cash interest of this broker is left out "
                 "of the XML.\n"));
    EXPECT_FALSE(contains(result.mOut, "the investment is left out of the XML"));
    EXPECT_TRUE(contains(folder.readOutput("Doh-Obr_2023.xml"), "<Value>10.00</Value>"));
}

TEST(ReportCommandTest, ExcludingTheOnlyBrokerLeavesDohObrWithNoData) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2023-02-01T10:00:00Z,2023-02-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,1.33,,,EUR,10.00,HRK,0.133000,Synthetic broker interest,synthetic-interest-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2023",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history},
                            "exclude\n");

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_FALSE(folder.hasOutput("Doh-Obr_2023.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "  Other interest (Doh-Obr): no data, nothing is left to report after "
                         "the excluded brokers\n"));
}

TEST(ReportCommandTest, ExcludingTheOnlyCouponInvestmentLeavesDohObrWithNoData) {
    // The HRK coupon of 2023-03-10 has no rate (the euro replaced HRK on 2023-01-01).
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2023-03-10T10:00:00Z,2023-03-10,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond A,XS0000000001,,,1.33,,,EUR,10.00,HRK,0.133000,Interest payment,synthetic-coupon-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2023",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history},
                            "exclude\n");

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_FALSE(folder.hasOutput("Doh-Obr_2023.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "  Other interest (Doh-Obr): no data, nothing is left to report after "
                         "the excluded investments\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Excluded investments (not in Doh-Obr): 1\n"
                         "  XS0000000001 Synthetic Bond A: missing_exchange_rate\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "    Resolved by your answer: the investment is left out of the XML.\n"));
}

TEST(ReportCommandTest, AnExcludedInvestmentAndAnExcludedBrokerAreCountedTogether) {
    // Both the HRK coupon of Bond A and the HRK cash interest of the broker have no rate; a EUR
    // coupon of Bond B stays.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2023-02-01T10:00:00Z,2023-02-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,1.33,,,EUR,10.00,HRK,0.133000,Synthetic broker interest,synthetic-interest-1,,,,)",
            R"(2023-03-10T10:00:00Z,2023-03-10,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond A,XS0000000001,,,1.33,,,EUR,10.00,HRK,0.133000,Interest payment,synthetic-coupon-1,,,,)",
            R"(2023-04-10T10:00:00Z,2023-04-10,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond B,XS0000000002,,,10.00,,,EUR,,,,Interest payment,synthetic-coupon-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2023",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history},
                            "exclude\n");

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "  Other interest (Doh-Obr): generated without 1 excluded investment and "
                         "1 excluded broker, 1 row, written to Doh-Obr_2023.xml\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Excluded investments (not in Doh-Obr): 1\n"
                         "  XS0000000001 Synthetic Bond A: missing_exchange_rate\n"));
    EXPECT_TRUE(contains(result.mOut, "Excluded brokers (not in Doh-Obr): Trade Republic\n"));
}

TEST(ReportCommandTest, DepositInterestAnsweredAtMostTheAllowanceWritesNoDohDho) {
    // Trade Republic is a bank from 6 December 2023, so this EUR 1.00 is deposit interest. The
    // user answers no: all banks together are at most EUR 1,000, so there is nothing to file.
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-04-01T10:00:00Z,2025-04-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,1.00,,,EUR,,,,Synthetic bank interest,synthetic-interest-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history},
                            "no\n");

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_FALSE(folder.hasOutput("Doh-DHO_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "  Bank-deposit interest (Doh-DHO): no data, not needed, your deposit "
                         "interest of all banks is at most EUR 1,000\n"));
}

// Rows that belong to no form

TEST(ReportCommandTest, APrivateMarketBuyWithUnitsIsListedUnderNotAboutAForm) {
    const TemporaryFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            // shares is 100: a private-market row has no units, so this one gets a warning.
            R"(2024-05-02T09:00:00Z,2024-05-02,DEFAULT,CASH,PRIVATE_MARKET_BUY,PRIVATE_FUND,Synthetic Private Fund,XX0000000009,100,1,-100,,,EUR,,,,,synthetic-private-buy-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--trade-republic",
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "\nNot about a form (skipped private-market rows): 1\n"
                         "  diagnostic-0001 warning unsupported_asset_class, history.csv row 2\n"));
}

} // namespace
