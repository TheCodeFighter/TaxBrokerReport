#include "cli/report_command.hpp"

#include "cli/decision_prompts.hpp"
#include "cli/output_files.hpp"
#include "cli/taxpayer_file.hpp"
#include "cli_text.hpp"
#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/application_request.hpp"
#include "taxbroker/application_run.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/currency.hpp"
#include "taxbroker/decision_session.hpp"
#include "taxbroker/input_file.hpp"
#include "taxbroker/official_rates.hpp"
#include "taxbroker/reference_catalogue.hpp"
#include "utils/numeric_util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <istream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace taxbroker::cli {
namespace {

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

constexpr std::string_view kReportHelp =
    R"(Usage:
  taxbroker report --year YYYY --taxpayer FILE [--self-report] [--no-input]
                   [--ecb-rates FILE...] [--reference-files FILE...] --out DIR
                   --trade-republic FILE...

Writes the forms Doh-KDVP (capital gains from selling shares and funds), Doh-Div (dividends),
Doh-DHO (bank-deposit interest) and Doh-Obr (other interest and bond coupons) for one year
from Trade Republic CSV exports, and a readable summary. The XML can be imported into eDavki
as a draft. Nothing is sent anywhere: everything stays on this computer. Each form is
made on its own: a blocked form never stops another.

Options:
  --year YYYY        The tax year, 2000 to 9999.
  --taxpayer FILE    Your details as JSON (see "Taxpayer file" below).
  --self-report      Write a self-report (samoprijava) instead of an original return.
                     Only Doh-KDVP has one: the income forms are always originals.
  --no-input         Ask no questions: an open question blocks its form (see
                     "Questions" below).
  --out DIR          The folder for the output files. It is created if it is missing.
  -h, --help         Show this help.

File options take one or more files. The list ends at the next option, so no other
argument may stand alone. The same option may be given again; its files are added. A file
whose name starts with "-" is given as ./-name.csv.
  --trade-republic FILE...
                     One or more Trade Republic exports. Give your whole history, not
                     only the tax year: FIFO needs the older purchases. Overlapping files
                     are fine; a transaction in two files is counted once.
  --ecb-rates FILE...
                     ECB reference-rate files (CSV from the ECB Data Portal, dataset EXR),
                     one per calendar year. The years 2016 to 2025 are built in; a file for
                     one of those years replaces the built-in year, and other years are
                     added. Needed to convert a trade in a foreign currency to EUR; see
                     "Foreign currencies" below.
  --reference-files FILE...
                     A newer payers.csv, relief_statements.csv or broker_payers.csv. Each
                     replaces the built-in file of the same name. Another file name is an
                     error.

Taxpayer file (JSON):
  {
    "taxNumber": "12345678",
    "isResident": true,
    "name": "Ana Novak",
    "address": "Slovenska cesta 1",
    "city": "Ljubljana",
    "postCode": "1000",
    "postName": "Ljubljana",
    "birthDate": "1990-01-31",
    "email": "ana.novak@example.com",
    "phoneNumber": "040123456"
  }
  Required: taxNumber (eight digits) and isResident (only true is supported for now).
  The other keys are optional. Any other key is refused, so a typo is never ignored.
  The filing type is not in the file: use --self-report for a self-report.

Output files in DIR:
  Doh-KDVP_YYYY.xml, Doh-Div_YYYY.xml, Doh-DHO_YYYY.xml, Doh-Obr_YYYY.xml
                     One file per form, written only when that form could be made in
                     full. A form without data has no file.
  summary_YYYY.txt   The status of each form, each investment sold in the year, the units
                     past the holding period, the skipped private-market events, the
                     warnings and errors of each form, the open questions and your
                     answers. The same summary is printed here.
  A form that is not made leaves the file of an earlier run as it is; the summary says
  so, so it is never mistaken for this run's result.
  While it writes, the command uses the names <file>.tmp and <file>.old in DIR, and may
  replace files with those names.

Supported for now:
  - Shares and funds (ETFs) bought and sold in EUR.
  - Units held longer than the holding period (15 years for a sale from 2022, 20 years
    before) are tax free: they are left out of the XML and listed in the summary.
  - The 30-day loss rule: a sale at a loss whose loss a purchase of the same investment
    within 30 days before or after it blocks in full is written with F10 false. A loss
    that the files alone cannot decide is written as you answer (see "Questions").
  - Dividends (Doh-Div), Trade Republic interest (Doh-DHO or Doh-Obr) and bond coupons
    (Doh-Obr), in EUR or a currency with an official ECB rate.

Questions:
  Some cases need your answer. The command asks in the terminal, one question at a time,
  and calculates again after each answer:
  - A sale at a loss that the files alone cannot decide: whether, within 30 days before or
    after the sale, you also bought or sold the investment anywhere that is not in your
    files (another broker, another account, or after the export). yes: the open loss is
    not deductible (F10 false); no: it is deductible.
  - A corporate action, for example a split: the ratio from the announcement (new shares
    for old shares, or your shares just before and just after it) and its effective date.
    You then confirm the position after it. A sale with too few units and a split row at
    most 10 days after it ask the same question, because the broker may have booked the
    split after it took effect. So do a split row in January within 30 days after a sale
    at a loss in December, and split rows of two brokers at most 10 days apart, which you
    first confirm as one split.
  - An asset class that is missing, not recognized or differs between rows (the question
    names the labels in the files): capital: report the investment as shares or fund
    units. exclude: leave it out of the reports; the summary lists it under "Excluded
    items" and you report it yourself if it is taxable. This question comes first.
  - Investments you could leave out: when each problem that blocks the report is tied to a
    single investment sold in the year (one or several, for example earlier purchases are
    missing), the command asks one question that lists them all. exclude: write Doh-KDVP
    without them; they are not in the XML and the summary lists them. fail: write no
    Doh-KDVP. A problem with a file, a row, the year or the XML is not solved this way. It
    comes after the questions of other investments and before those of the listed ones,
    which exclude drops.
  - Income questions (dividends, interest and bond coupons). They are:
    the type of a bond sold or matured in the year (ordinary, or discount for a discount or
    zero-coupon bond); whether your bank-deposit interest with your other banks is above
    EUR 1,000 (without an answer Doh-DHO is made, from the imported interest only); which
    of several reference rows is the payer of a dividend (a number, or skip: you complete
    the payer in eDavki); what an income payment of unknown type is; and, as for Doh-KDVP,
    whether to leave out the dividend or coupon investments with a problem (exclude, fail
    or cancel).
  An answer that does not fit is explained and the question is asked again. cancel closes
  a question without an answer, and the form it is about is blocked. With --no-input, or when
  the input ends (Ctrl-D), the open questions stay open: the forms they are about are blocked
  (a payer choice, a treaty claim and the deposit-interest question block nothing) and the
  summary lists them. The answers are used only in this run and are never saved, so the next run
  asks again.

Foreign currencies:
  The unit price of a trade in another currency is converted to EUR at the official rate of
  the day of that trade: 1 EUR = rate units of the currency (the ECB reference rate, the
  same as the Banka Slovenije rate). The rates of 2016 to 2025 are built in. For a year
  without built-in rates, or to use your own file, give the ECB file of that year with
  --ecb-rates. Every year that has such a trade up to 31 December of the tax year counts,
  also an earlier one: older purchases are part of FIFO.
  A day without an ECB rate (a weekend or holiday) takes the last earlier rate, at most
  4 days back, with a warning. The summary lists the rates used. Trades after the tax year
  need no rate. When a file is missing or ends too early, the summary lists every year to
  load, so one more run is enough.

Left out by a tax rule (not in the XML, listed under "Excluded items" in the summary,
without a warning):
  - Bonds: debt securities are no capital gain. Only their interest is reported (Doh-Obr).
    The summary shows the units bought, sold and matured in the year.
  - Crypto assets: they are not part of Doh-KDVP. Check whether a separate crypto return
    applies to you.

Doh-KDVP is blocked (no XML) when an investment sold in the year:
  - is a private fund or a derivative;
  - has a split or reverse split up to 31 December whose ratio is not answered, a merger,
    or split rows of two brokers at most 10 days apart that are not one split. An answered
    split is applied;
  - misses earlier purchase history;
  - has a corporate action without an answered ratio, or a merger, within 30 days after a
    sale at a loss in December;
  - has a split or reverse split that makes the value of one unit too large for the form;
  - has a trade in a foreign currency that no ECB rate file covers (no built-in or loaded
    file for the year, a file that ends before the day, no rate near the day, or a
    currency that the euro replaced);
  - has a question that is open or was cancelled.
It is also blocked when a file cannot be read, when a trade or corporate-action row of
any file is rejected, when two files disagree about the same trade, when the asset class
question of an investment with a row up to 31 December is open or cancelled (sold in the
year or not), or when a value does not fit the form (for example a postCode that is too
long).

Skipped:
  - Private-market events (Trade Republic types PRIVATE_MARKET_BUY and
    PRIVATE_MARKET_SELL) have only a cash amount, without units or a price, so they are
    not processed. Each one in the year gets a warning in the summary: check whether you
    have to report it yourself. A private-market row that cannot be read is listed with
    its error, but it does not block the report. A row that has units or a price gets
    one more warning, because those units are not used.
  - Bonus credits (Trade Republic types BENEFITS_SAVEBACK, STOCKPERK, BONUS and
    REFERRAL) are information only. They are in no form and never change a purchase value.
  - Bond maturities and their corrections (Trade Republic FINAL_MATURITY,
    FINAL_MATURITY_CANCELLED and REDEMPTION rows) are information only. Bond sales and
    maturities are in no form; the summary adds them up per bond.

Doh-Div, Doh-DHO and Doh-Obr are blocked, for example, by a payment in a foreign currency
that no ECB rate file covers, by an open question or by a payment that is rejected.

Exit codes:
  0  Every form was written, or has no data.
  1  At least one form is blocked: its errors and open questions are in the summary. The
     other forms are still written.
  2  The command could not run: a wrong option, a missing file, an invalid taxpayer
     file or an output file that cannot be written. No output file was changed.

Example:
  taxbroker report --year 2025 --taxpayer taxpayer.json --out out --trade-republic export.csv
)";

constexpr std::string_view kHelpHint = "Run 'taxbroker report --help' for the usage.\n";

/// What the command line of `report` asks for.
struct ReportOptions {
    int mYear{};
    std::filesystem::path mTaxpayerPath;
    FilingType mFilingType{FilingType::Original};
    bool mAskQuestions{true}; ///< False with --no-input: the open questions stay open.
    std::filesystem::path mOutputDirectory;
    std::vector<std::filesystem::path> mTradeRepublicPaths; ///< The --trade-republic files.
    std::vector<std::filesystem::path> mEcbRatePaths;       ///< The --ecb-rates files.
    std::vector<std::filesystem::path> mReferencePaths;     ///< The --reference-files files.
};

/// A problem that stops the run before anything is written, with its text for stderr.
struct UsageError {
    std::string mMessage;
};

struct HelpRequested {};

/// Only the last part of a path, so no folder of the user is printed. A path that ends in a
/// separator has no file name; a placeholder is printed then.
std::string fileNameOf(const std::filesystem::path& aPath) {
    const std::string name = aPath.filename().string();

    return name.empty() ? "<no file name>" : name;
}

std::optional<int> parseYear(std::string_view aText) {
    int year{};
    const auto* end = aText.data() + aText.size();
    const auto parsed = std::from_chars(aText.data(), end, year);

    if (aText.empty() || aText.front() == '+' || parsed.ec != std::errc{} || parsed.ptr != end ||
        year < kMinimumReportingYear || year > kMaximumReportingYear)
    {
        return std::nullopt;
    }

    return year;
}

/// Reads the options of `report`. The options come in any order. A file option takes the
/// arguments after it up to the next option; any other argument is an error.
std::variant<ReportOptions, UsageError, HelpRequested>
parseReportArguments(std::span<const std::string> aArguments) {
    ReportOptions options;
    std::optional<std::string> yearText;
    std::optional<std::string> taxpayerText;
    std::optional<std::string> outputText;
    bool selfReport = false;
    bool noInput = false;

    // The list that takes the next plain arguments, its option and how many it has so far.
    std::vector<std::filesystem::path>* list = nullptr;
    std::string listOption;
    std::size_t listFiles = 0;

    for (std::size_t index = 0; index < aArguments.size(); ++index)
    {
        const std::string& argument = aArguments[index];

        if (argument.empty() || argument.front() != '-')
        {
            if (list == nullptr)
            {
                return UsageError{
                    (argument.empty() ? std::string{"An empty argument"} : fileNameOf(argument)) +
                    " is not after a file option such as --trade-republic."};
            }

            list->emplace_back(argument);
            ++listFiles;
            continue;
        }

        if (argument == "-h" || argument == "--help")
        {
            return HelpRequested{};
        }

        // An option ends the list before it.
        if (list != nullptr && listFiles == 0)
        {
            return UsageError{listOption + " needs at least one file."};
        }

        list = nullptr;

        if (argument == "--trade-republic")
        {
            list = &options.mTradeRepublicPaths;
        }
        else if (argument == "--ecb-rates")
        {
            list = &options.mEcbRatePaths;
        }
        else if (argument == "--reference-files")
        {
            list = &options.mReferencePaths;
        }

        if (list != nullptr)
        {
            listOption = argument;
            listFiles = 0;
            continue;
        }

        if (argument == "--self-report")
        {
            if (selfReport)
            {
                return UsageError{"--self-report is given twice."};
            }

            selfReport = true;
            continue;
        }

        if (argument == "--no-input")
        {
            if (noInput)
            {
                return UsageError{"--no-input is given twice."};
            }

            noInput = true;
            continue;
        }

        std::optional<std::string>* target = nullptr;

        if (argument == "--year")
        {
            target = &yearText;
        }
        else if (argument == "--taxpayer")
        {
            target = &taxpayerText;
        }
        else if (argument == "--out")
        {
            target = &outputText;
        }
        else
        {
            // Only the name before "=", so a value such as a folder path is never printed.
            const auto equals = argument.find('=');

            return UsageError{"Unknown option " + argument.substr(0, equals) +
                              (equals == std::string::npos ? "." : "=...")};
        }

        if (target->has_value())
        {
            return UsageError{argument + " is given twice."};
        }

        if (index + 1 == aArguments.size())
        {
            return UsageError{argument + " needs a value."};
        }

        *target = aArguments[++index];
    }

    if (list != nullptr && listFiles == 0)
    {
        return UsageError{listOption + " needs at least one file."};
    }

    if (!yearText)
    {
        return UsageError{"--year is missing."};
    }

    const auto year = parseYear(*yearText);

    if (!year)
    {
        return UsageError{"--year must be a year from " + std::to_string(kMinimumReportingYear) +
                          " to " + std::to_string(kMaximumReportingYear) + "."};
    }

    if (!taxpayerText)
    {
        return UsageError{"--taxpayer is missing."};
    }

    if (!outputText)
    {
        return UsageError{"--out is missing."};
    }

    if (options.mTradeRepublicPaths.empty())
    {
        return UsageError{"Give at least one Trade Republic CSV file with --trade-republic."};
    }

    options.mYear = *year;
    options.mTaxpayerPath = *taxpayerText;
    options.mOutputDirectory = *outputText;
    options.mFilingType = selfReport ? FilingType::SelfReport : FilingType::Original;
    options.mAskQuestions = !noInput;

    return options;
}

/// Checks the files the run needs before any processing.
std::optional<UsageError> checkInputFiles(const ReportOptions& aOptions) {
    std::error_code error;
    const auto missingCsv = std::find_if(aOptions.mTradeRepublicPaths.begin(),
                                         aOptions.mTradeRepublicPaths.end(),
                                         [&error](const std::filesystem::path& aPath) {
                                             return !std::filesystem::is_regular_file(aPath, error);
                                         });

    if (missingCsv != aOptions.mTradeRepublicPaths.end())
    {
        return UsageError{"The CSV file " + fileNameOf(*missingCsv) +
                          " does not exist or is not a file."};
    }

    const auto missingRates =
        std::find_if(aOptions.mEcbRatePaths.begin(),
                     aOptions.mEcbRatePaths.end(),
                     [&error](const std::filesystem::path& aPath) {
                         return !std::filesystem::is_regular_file(aPath, error);
                     });

    if (missingRates != aOptions.mEcbRatePaths.end())
    {
        return UsageError{"The ECB rate file " + fileNameOf(*missingRates) +
                          " does not exist or is not a file."};
    }

    const auto missingReference =
        std::find_if(aOptions.mReferencePaths.begin(),
                     aOptions.mReferencePaths.end(),
                     [&error](const std::filesystem::path& aPath) {
                         return !std::filesystem::is_regular_file(aPath, error);
                     });

    if (missingReference != aOptions.mReferencePaths.end())
    {
        return UsageError{"The reference file " + fileNameOf(*missingReference) +
                          " does not exist or is not a file."};
    }

    if (!std::filesystem::is_regular_file(aOptions.mTaxpayerPath, error))
    {
        return UsageError{"The taxpayer file " + fileNameOf(aOptions.mTaxpayerPath) +
                          " does not exist or is not a file."};
    }

    if (std::filesystem::exists(aOptions.mOutputDirectory, error) &&
        !std::filesystem::is_directory(aOptions.mOutputDirectory, error))
    {
        return UsageError{"--out must be a folder, not a file."};
    }

    return std::nullopt;
}

std::string isoDate(const Date& aDate) {
    const std::chrono::year_month_day day{std::chrono::sys_days{aDate}};
    char text[16]{};

    std::snprintf(text,
                  sizeof(text),
                  "%04d-%02u-%02u",
                  static_cast<int>(day.year()),
                  static_cast<unsigned>(day.month()),
                  static_cast<unsigned>(day.day()));

    return text;
}

/// The run-local ID of a file, as in the application result: `source-0001` for index 0.
std::string sourceId(std::size_t aSourceIndex) {
    char text[32]{};

    std::snprintf(text, sizeof(text), "source-%04zu", aSourceIndex + 1);

    return text;
}

std::string sourceText(const ApplicationSource& aSource) {
    const std::string file =
        aSource.mFilename ? aSource.mFilename->value() : sourceId(aSource.mSourceIndex);

    return aSource.mRow ? file + " row " + std::to_string(*aSource.mRow) : file;
}

bool affects(const ApplicationDiagnostic& aDiagnostic, ReportType aType) {
    return std::find(aDiagnostic.mAffectedReports.begin(),
                     aDiagnostic.mAffectedReports.end(),
                     aType) != aDiagnostic.mAffectedReports.end();
}

std::string_view severityText(DiagnosticSeverity aSeverity) {
    switch (aSeverity)
    {
    case DiagnosticSeverity::Warning:
        return "warning";
    case DiagnosticSeverity::Error:
        break;
    }

    return "error";
}

/// One diagnostic in two lines: where it is, then the English message. The severity is written
/// only for a list that mixes errors and warnings. aForm is the form the list belongs to, if any.
void writeDiagnostic(std::ostream& aOut,
                     const ApplicationDiagnostic& aDiagnostic,
                     bool aWithSeverity,
                     std::optional<ReportType> aForm) {
    aOut << "  " << aDiagnostic.mId << ' ';

    if (aWithSeverity)
    {
        aOut << severityText(aDiagnostic.mSeverity) << ' ';
    }

    aOut << api::diagnosticCodeName(aDiagnostic.mCode);

    if (aDiagnostic.mIsin || aDiagnostic.mInstrumentName)
    {
        aOut << ", "
             << investmentText(aDiagnostic.mIsin.value_or(""),
                               aDiagnostic.mInstrumentName.value_or(""));
    }

    if (aDiagnostic.mTaxDate)
    {
        aOut << ", " << isoDate(*aDiagnostic.mTaxDate);
    }

    if (!aDiagnostic.mSources.empty())
    {
        aOut << ", " << sourceText(aDiagnostic.mSources.front());

        if (aDiagnostic.mSources.size() > 1)
        {
            aOut << " (+" << aDiagnostic.mSources.size() - 1 << " more)";
        }
    }

    aOut << "\n    " << aDiagnostic.mMessage << '\n';

    // An error that does not block was resolved by the user leaving its investment (or a broker's
    // cash interest) out, unless it is a class error of a file row that the classification of its
    // investment took over.
    if (aDiagnostic.mSeverity == DiagnosticSeverity::Error && !aDiagnostic.mBlocking && aForm &&
        affects(aDiagnostic, *aForm))
    {
        const bool isFileError = aDiagnostic.mStage == PipelineStage::Parsing ||
                                 aDiagnostic.mStage == PipelineStage::Merging;

        if (isFileError)
        {
            aOut << "    Handled by the classification of its investment: see the question or "
                    "the excluded items.\n";
        }
        else if (aDiagnostic.mCashInterestBroker)
        {
            aOut << "    Resolved by your answer: the cash interest of this broker is left out of "
                    "the XML.\n";
        }
        else
        {
            aOut << "    Resolved by your answer: the investment is left out of the XML.\n";
        }
    }
}

void writeDiagnostics(std::ostream& aOut,
                      std::string_view aTitle,
                      const std::vector<const ApplicationDiagnostic*>& aDiagnostics,
                      bool aWithSeverity,
                      std::optional<ReportType> aForm) {
    aOut << aTitle << ": " << aDiagnostics.size() << '\n';

    for (const auto* diagnostic : aDiagnostics)
    {
        writeDiagnostic(aOut, *diagnostic, aWithSeverity, aForm);
    }
}

/// Everything the summary needs from one run: the request, the result, and which forms keep a
/// file of an earlier run in the output folder.
struct RunOutcome {
    const ApplicationRequest& mRequest;
    const ApplicationResult& mResult;
    /// True for a form that is not generated and has an XML file of an earlier run in the folder.
    std::array<bool, 4> mKeptOldFile{};
};

/// One tax form of the report: its result type, its code in the file name and its label.
struct FormInfo {
    ReportType mType{};      ///< Always set by the creator.
    std::string_view mCode;  ///< For example "Doh-KDVP".
    std::string_view mLabel; ///< For example "Capital gains".
};

/// The forms in the fixed order of the report types.
constexpr std::array<FormInfo, 4> kForms{{
    {ReportType::CapitalGains, "Doh-KDVP", "Capital gains"},
    {ReportType::Dividends, "Doh-Div", "Dividends"},
    {ReportType::BankDepositInterest, "Doh-DHO", "Bank-deposit interest"},
    {ReportType::OtherInterest, "Doh-Obr", "Other interest"},
}};

std::string xmlFileName(const FormInfo& aForm, int aYear) {
    return std::string{aForm.mCode} + "_" + std::to_string(aYear) + ".xml";
}

std::string summaryFileName(int aYear) {
    return "summary_" + std::to_string(aYear) + ".txt";
}

/// The text of a text parameter of the diagnostic, or an empty string when it has none.
std::string textParameter(const ApplicationDiagnostic& aDiagnostic, std::string_view aName) {
    const auto found =
        std::find_if(aDiagnostic.mMessageParams.begin(),
                     aDiagnostic.mMessageParams.end(),
                     [aName](const MessageParam& aParam) { return aParam.mName == aName; });

    return found == aDiagnostic.mMessageParams.end() ? std::string{}
                                                     : std::get<std::string>(found->mValue);
}

/// The number parameter of the diagnostic, or nothing when it has none.
std::optional<std::uint64_t> numberParameter(const ApplicationDiagnostic& aDiagnostic,
                                             std::string_view aName) {
    const auto found =
        std::find_if(aDiagnostic.mMessageParams.begin(),
                     aDiagnostic.mMessageParams.end(),
                     [aName](const MessageParam& aParam) { return aParam.mName == aName; });

    if (found == aDiagnostic.mMessageParams.end() ||
        !std::holds_alternative<std::uint64_t>(found->mValue))
    {
        return std::nullopt;
    }

    return std::get<std::uint64_t>(found->mValue);
}

/// Adds the years of a "2023, 2024" text to the set.
void addYears(std::set<int>& aYears, const std::string& aText) {
    std::istringstream stream{aText};
    std::string part;

    while (std::getline(stream, part, ','))
    {
        const auto start = part.find_first_not_of(' ');

        if (start == std::string::npos)
        {
            continue;
        }

        int year = 0;
        const auto* first = part.data() + start;
        const auto* last = part.data() + part.size();

        if (std::from_chars(first, last, year).ec == std::errc{})
        {
            aYears.insert(year);
        }
    }
}

/// True when the diagnostic is about an income form.
bool affectsIncomeForm(const ApplicationDiagnostic& aDiagnostic) {
    return affects(aDiagnostic, ReportType::Dividends) ||
           affects(aDiagnostic, ReportType::BankDepositInterest) ||
           affects(aDiagnostic, ReportType::OtherInterest);
}

/// The years of the ECB rate files to load or to replace. The capital years come from its run
/// warnings; an income form has no such warning, so its years come from the structured parameters
/// of its `missing_exchange_rate` errors.
void writeRateFileYears(std::ostream& aOut, const ApplicationResult& aResult) {
    std::set<int> toLoad;
    std::set<int> toReplace;

    for (const auto& diagnostic : aResult.mDiagnostics)
    {
        if (diagnostic.mMessageKey == MessageKey::OfficialRateFilesRequiredNotLoaded)
        {
            addYears(toLoad, textParameter(diagnostic, "years"));
        }
        else if (diagnostic.mMessageKey == MessageKey::OfficialRateFilesRequiredEndsEarly)
        {
            addYears(toReplace, textParameter(diagnostic, "years"));
        }
        else if (diagnostic.mMessageKey == MessageKey::MissingExchangeRateNoRateFile &&
                 affectsIncomeForm(diagnostic))
        {
            if (const auto year = numberParameter(diagnostic, "year"))
            {
                toLoad.insert(static_cast<int>(*year));
            }
        }
        else if (diagnostic.mMessageKey == MessageKey::MissingExchangeRateRateFileEndsEarly &&
                 affectsIncomeForm(diagnostic))
        {
            // The file of the date's year ends early, so its last date has that year.
            addYears(toReplace, textParameter(diagnostic, "lastDate").substr(0, 4));
        }
    }

    const auto text = [](const std::set<int>& aYears) {
        std::string years;

        for (const auto year : aYears)
        {
            years += (years.empty() ? "" : ", ") + std::to_string(year);
        }

        return years;
    };

    if (!toLoad.empty())
    {
        aOut << "  ECB rate files to load: " << text(toLoad) << '\n';
    }

    if (!toReplace.empty())
    {
        aOut << "  ECB rate files to replace (they end too early): " << text(toReplace) << '\n';
    }
}

/// " without 1 excluded investment and 1 excluded broker" for what the user left out of a form,
/// or nothing.
std::string leftOutText(const ReportResult& aReport) {
    std::string text;

    if (!aReport.mExcludedInvestments.empty())
    {
        text += " without " + countText(aReport.mExcludedInvestments.size(),
                                        "excluded investment",
                                        "excluded investments");
    }

    if (!aReport.mExcludedBrokers.empty())
    {
        const auto brokers =
            countText(aReport.mExcludedBrokers.size(), "excluded broker", "excluded brokers");

        text += (text.empty() ? " without " : " and ") + brokers;
    }

    return text;
}

/// Why a form has no data, as the rest of its status line.
std::string noDataText(const FormInfo& aForm, const ReportResult& aReport, int aYear) {
    if (!aReport.mExcludedInvestments.empty())
    {
        return "nothing is left to report after the excluded investments";
    }

    if (!aReport.mExcludedBrokers.empty())
    {
        return "nothing is left to report after the excluded brokers";
    }

    if (aReport.mNoDataReason == NoDataReason::BelowDepositInterestThreshold)
    {
        return "not needed, your deposit interest of all banks is at most EUR 1,000";
    }

    const auto year = std::to_string(aYear);

    switch (aForm.mType)
    {
    case ReportType::CapitalGains:
        return "no sale in " + year + " needs a Doh-KDVP form";
    case ReportType::Dividends:
        return "no dividend to report in " + year;
    case ReportType::BankDepositInterest:
        return "no bank-deposit interest to report in " + year;
    case ReportType::OtherInterest:
        break;
    }

    return "no cash interest or bond coupon to report in " + year;
}

/// One status line per form, and a note under a form whose old file was kept.
void writeForms(std::ostream& aOut, const RunOutcome& aOutcome) {
    const int year = aOutcome.mRequest.mReportingYear;

    aOut << "Forms:\n";

    for (const auto& form : kForms)
    {
        const auto& report = aOutcome.mResult.report(form.mType);
        const auto index = static_cast<std::size_t>(form.mType);
        const auto rows = report.mIncome ? report.mIncome->mRows.size() : std::size_t{0};

        aOut << "  " << form.mLabel << " (" << form.mCode << "): ";

        switch (report.mStatus)
        {
        case ReportStatus::Generated:
            aOut << "generated" << leftOutText(report);

            if (form.mType != ReportType::CapitalGains)
            {
                aOut << ", " << countText(rows, "row", "rows");
            }

            aOut << ", written to " << xmlFileName(form, year) << '\n';
            break;
        case ReportStatus::NoData:
            aOut << "no data, " << noDataText(form, report, year) << '\n';
            break;
        case ReportStatus::NeedsInput:
        case ReportStatus::Failed:
            aOut << "blocked, no XML was written (see the errors below)\n";
            break;
        }

        if (aOutcome.mKeptOldFile[index])
        {
            aOut << "    " << xmlFileName(form, year)
                 << " in the output folder is from an earlier run, not from this run.\n";
        }
    }

    writeRateFileYears(aOut, aOutcome.mResult);
}

void writeInvestments(std::ostream& aOut, const RunOutcome& aOutcome) {
    const auto& investments = aOutcome.mResult.mCapitalInvestments;
    const bool generated =
        aOutcome.mResult.report(ReportType::CapitalGains).mStatus == ReportStatus::Generated;

    aOut << "Investments with a sale in " << aOutcome.mRequest.mReportingYear << ": "
         << investments.size() << '\n';

    for (const auto& investment : investments)
    {
        aOut << "  " << investmentText(investment.mIsin, investment.mName) << ": ";

        switch (investment.mState)
        {
        case CapitalInvestmentState::Excluded:
        case CapitalInvestmentState::Blocked:
            aOut << (investment.mState == CapitalInvestmentState::Excluded ? "excluded ("
                                                                           : "blocked (")
                 << reasonCodesText(investment.mReasonCodes) << ")\n";

            for (const auto& sale : investment.mSales)
            {
                aOut << "    sale " << isoDate(sale.mDate) << ", " << sourceText(sale.mSource)
                     << '\n';
            }

            break;
        case CapitalInvestmentState::LongHeldOnly:
            aOut << "only units past the holding period, not in the XML\n";
            break;
        case CapitalInvestmentState::Listed:
            aOut << (generated ? "listed, " : "no problem, ")
                 << countText(investment.mListedSales, "sale", "sales") << ", "
                 << unitsText(investment.mListedUnits) << " units";

            if (investment.mBlockedLossUnits > 0)
            {
                aOut << ", " << unitsText(investment.mBlockedLossUnits)
                     << " units with a loss blocked by the 30-day rule (F10 false)";
            }

            aOut << '\n';
            break;
        }
    }
}

/// The investments and brokers the user left out of each form, only for forms that have some.
void writeExcluded(std::ostream& aOut, const RunOutcome& aOutcome) {
    for (const auto& form : kForms)
    {
        const auto& report = aOutcome.mResult.report(form.mType);

        if (!report.mExcludedInvestments.empty())
        {
            // The capital list keeps its own heading: the form is the one of the summary.
            aOut << "Excluded investments (not in "
                 << (form.mType == ReportType::CapitalGains ? "the XML" : form.mCode)
                 << "): " << report.mExcludedInvestments.size() << '\n';

            for (const auto& investment : report.mExcludedInvestments)
            {
                aOut << "  " << investmentText(investment.mIsin, investment.mName) << ": "
                     << reasonCodesText(investment) << '\n';
            }

            aOut << '\n';
        }

        if (!report.mExcludedBrokers.empty())
        {
            aOut << "Excluded brokers (not in " << form.mCode << "): ";

            for (std::size_t index = 0; index < report.mExcludedBrokers.size(); ++index)
            {
                aOut << (index == 0 ? "" : ", ")
                     << brokerName(report.mExcludedBrokers[index].mBroker);
            }

            aOut << "\n\n";
        }
    }
}

std::string_view categoryText(ExcludedCategory aCategory) {
    switch (aCategory)
    {
    case ExcludedCategory::Bond:
        return "bond";
    case ExcludedCategory::Crypto:
        return "crypto";
    case ExcludedCategory::Dividend:
        return "dividend";
    case ExcludedCategory::Interest:
        return "interest";
    case ExcludedCategory::Unknown:
        break;
    }

    return "unknown class";
}

std::string_view reasonText(ExcludedReason aReason) {
    switch (aReason)
    {
    case ExcludedReason::DebtSecurity:
        // Known gap until future task: interest of a discount or zero-coupon bond on a sale or
        // maturity is not calculated yet, and no warning says so.
        return "tax rule: debt security, only its interest is taxable";
    case ExcludedReason::CryptoNotReported:
        return "tax rule: crypto is not part of Doh-KDVP; check whether a separate crypto "
               "return applies to you";
    case ExcludedReason::SlovenianTaxSettled:
        return "tax rule: a Slovenian payer deducts the tax itself; sales of its shares stay in "
               "Doh-KDVP";
    case ExcludedReason::UserChoice:
        break;
    }

    return "your choice";
}

/// The instruments left out by a tax rule or by the user's answer, only when there is one.
void writeExcludedItems(std::ostream& aOut, const ApplicationResult& aResult) {
    if (aResult.mExcludedItems.empty())
    {
        return;
    }

    aOut << "Excluded items (left out of the reports, not in the XML): "
         << aResult.mExcludedItems.size() << '\n';

    for (const auto& item : aResult.mExcludedItems)
    {
        aOut << "  " << investmentText(item.mIsin, item.mName) << ": "
             << categoryText(item.mCategory) << ", " << reasonText(item.mReason);

        if (item.mBond)
        {
            aOut << "; bought " << unitsText(item.mBond->mBoughtUnits) << ", sold "
                 << unitsText(item.mBond->mSoldUnits) << ", matured "
                 << unitsText(item.mBond->mMaturedUnits) << " units";

            if (item.mBond->mMaturityDate)
            {
                aOut << ", maturity " << isoDate(*item.mBond->mMaturityDate);
            }
        }

        aOut << '\n';
    }

    aOut << '\n';
}

void writeLongHeld(std::ostream& aOut, const RunOutcome& aOutcome) {
    const auto& longHeld = aOutcome.mResult.mLongHeld;

    aOut << "Units past the holding period (tax free, not in the XML): " << longHeld.size() << '\n';

    for (const auto& sale : longHeld)
    {
        aOut << "  " << investmentText(sale.mIsin, sale.mName) << ": sale "
             << isoDate(sale.mSaleDate) << ", " << unitsText(sale.mUnits) << " units\n";
    }
}

void writePrivateMarketEvents(std::ostream& aOut, const RunOutcome& aOutcome) {
    std::vector<const ApplicationDiagnostic*> skipped;

    for (const auto& diagnostic : aOutcome.mResult.mDiagnostics)
    {
        if (diagnostic.mMessageKey == MessageKey::UnsupportedTaxTreatmentPrivateMarketSkipped)
        {
            skipped.push_back(&diagnostic);
        }
    }

    aOut << "Private-market events skipped (only a cash amount, no units): " << skipped.size()
         << '\n';

    for (const auto* diagnostic : skipped)
    {
        aOut << "  "
             << investmentText(diagnostic->mIsin.value_or("(no ISIN)"),
                               diagnostic->mInstrumentName.value_or(""))
             << ": " << isoDate(diagnostic->mTaxDate.value()) << ", "
             << sourceText(diagnostic->mSources.front()) << '\n';
    }
}

/// The open requests, only when there is one.
void writeOpenQuestions(std::ostream& aOut, const ApplicationResult& aResult) {
    if (aResult.mRequests.empty())
    {
        return;
    }

    aOut << "\nOpen questions: " << aResult.mRequests.size() << '\n';

    const auto nameOf = [](const auto& aQuestion) -> std::string {
        using Question = std::decay_t<decltype(aQuestion)>;

        if constexpr (std::is_same_v<Question, std::monostate>)
        {
            return {};
        }
        else if constexpr (std::is_same_v<Question, FailureResolutionQuestion>)
        {
            // It lists several investments, not one.
            return unitsText(aQuestion.mInvestments.size(), aQuestion.mBrokers.size());
        }
        else if constexpr (std::is_same_v<Question, LossEligibilityQuestion>)
        {
            return aQuestion.mName;
        }
        else if constexpr (std::is_same_v<Question, TaxClassificationQuestion>)
        {
            return aQuestion.mName;
        }
        else if constexpr (std::is_same_v<Question, QuantityCorrectionQuestion> ||
                           std::is_same_v<Question, PayerChoiceQuestion> ||
                           std::is_same_v<Question, IncomeClassificationQuestion> ||
                           std::is_same_v<Question, BondTypeQuestion>)
        {
            return aQuestion.mName;
        }
        else if constexpr (std::is_same_v<Question, TreatyClaimQuestion>)
        {
            return "treaty claim for " + aQuestion.mCountry;
        }
        else if constexpr (std::is_same_v<Question, InterestTreatyClaimQuestion>)
        {
            return "interest treaty claim for " + aQuestion.mCountry;
        }
        else if constexpr (std::is_same_v<Question, DepositInterestTotalQuestion>)
        {
            return "bank-deposit interest";
        }
        else
        {
            static_assert(std::is_same_v<Question, CorporateActionRatioQuestion>);
            return aQuestion.mName;
        }
    };

    for (const auto& request : aResult.mRequests)
    {
        const auto name = std::visit(nameOf, request.mData);

        aOut << "  " << request.mId << ' ' << api::requestTypeName(request.mType) << ", "
             << investmentText(request.mIsin.value_or(""), name) << '\n';
    }

    aOut << "  Run the command again in a terminal (without --no-input) to answer them.\n";
}

/// The answers given in this run, only when there is one. They are never saved, so this run is
/// the only one that has them.
void writeAnswers(std::ostream& aOut, const ApplicationResult& aResult) {
    if (aResult.mDecisions.empty())
    {
        return;
    }

    aOut << "\nAnswered in this run: " << aResult.mDecisions.size() << '\n';

    for (const auto& decision : aResult.mDecisions)
    {
        aOut << "  " << answerLine(decision) << '\n';
    }
}

/// The official rates that converted a trade, so the user can check them. The date of the rate
/// is named only when it is earlier than the trade's tax date.
void writeUsedRates(std::ostream& aOut, const RunOutcome& aOutcome) {
    const auto& usedRates = aOutcome.mResult.mUsedRates;

    if (usedRates.empty())
    {
        return;
    }

    aOut << "Official exchange rates used (ECB reference rates, 1 EUR = rate in the currency):\n";

    for (const auto& used : usedRates)
    {
        const auto& rate = used.mRate;

        const auto rateText = formatFixedPoint(rate.mValue, EXCHANGE_RATE_SCALE, 8);

        aOut << "  " << rate.mCurrencyCode << ' ' << isoDate(used.mTaxDate) << ": 1 EUR = ";
        aOut << rateText << ' ' << rate.mCurrencyCode;

        if (rate.mObservationDate != used.mTaxDate)
        {
            aOut << " (rate of " << isoDate(rate.mObservationDate) << ")";
        }

        aOut << " (" << rate.mSourceFile.value() << " row " << rate.mSourceRow
             << (rate.mOrigin == OfficialRateOrigin::BuiltIn ? ", built-in" : "") << ")\n";
    }

    aOut << '\n';
}

/// The benefit type as a word for the summary.
std::string_view benefitTypeText(BenefitType aType) {
    switch (aType)
    {
    case BenefitType::Saveback:
        return "saveback";
    case BenefitType::StockPerk:
        return "stock perk";
    case BenefitType::Bonus:
        return "bonus";
    case BenefitType::Referral:
        return "referral";
    }

    return "benefit";
}

/// Whether an amount in a currency other than EUR is among the totals.
bool hasForeignCurrency(const std::vector<CurrencyTotal>& aTotals) {
    return std::ranges::any_of(aTotals, [](const CurrencyTotal& aTotal) {
        return aTotal.mCurrency != Currency::EUR;
    });
}

/// An EUR total, or why it is not available: a foreign amount without an EUR value (aReason), or
/// otherwise a sum too large to keep.
std::string
eurTotalText(const std::optional<Money>& aTotal, bool aHasForeign, std::string_view aReason) {
    if (aTotal)
    {
        return formatFixedPoint(*aTotal, MONEY_SCALE, 4) + " EUR";
    }

    return "not available (" + std::string{aHasForeign ? aReason : "the sum is too large"} + ")";
}

/// One line per currency (`Trade Republic: 2 fees, 6.0000 EUR`), then the EUR total of the row
/// only when it has a foreign currency.
void writeSummaryRow(std::ostream& aOut,
                     const std::string& aLabel,
                     const std::vector<CurrencyTotal>& aTotals,
                     const std::optional<Money>& aEurTotal,
                     std::string_view aNoun,
                     std::string_view aReason) {
    for (const auto& total : aTotals)
    {
        aOut << "  " << aLabel << ": " << total.mCount << ' ' << aNoun
             << (total.mCount == 1 ? "" : "s") << ", "
             << (total.mTotal ? formatFixedPoint(*total.mTotal, MONEY_SCALE, 4) : "not available")
             << ' ' << currencyCode(total.mCurrency) << '\n';
    }

    if (hasForeignCurrency(aTotals))
    {
        aOut << "  " << aLabel << " in EUR: " << eurTotalText(aEurTotal, true, aReason) << '\n';
    }
}

/// The overall line: `Total: 6.0000 EUR`, or `Total in EUR: not available (...)`.
template <typename Row>
void writeSummaryTotal(std::ostream& aOut,
                       const std::vector<Row>& aRows,
                       const std::optional<Money>& aEurTotal,
                       std::string_view aReason) {
    if (aEurTotal)
    {
        aOut << "  Total: " << eurTotalText(aEurTotal, false, aReason) << '\n';

        return;
    }

    const bool hasForeign = std::ranges::any_of(aRows, [](const Row& aRow) {
        return hasForeignCurrency(aRow.mByCurrency);
    });

    aOut << "  Total in EUR: " << eurTotalText(aEurTotal, hasForeign, aReason) << '\n';
}

/// The fees and the benefits of the year. They are information only and are in no form.
void writeFeesAndBenefits(std::ostream& aOut, const RunOutcome& aOutcome) {
    constexpr std::string_view kFeeReason = "a fee has no broker rate";
    constexpr std::string_view kBenefitReason = "a benefit is not in EUR";
    const auto& fees = aOutcome.mResult.mFees;
    const auto& benefits = aOutcome.mResult.mBenefits;
    const auto year = std::to_string(aOutcome.mRequest.mReportingYear);

    aOut << "Fees in " << year << " (information only, not in any form):";

    if (fees.mBrokers.empty())
    {
        aOut << " none\n";
    }
    else
    {
        aOut << '\n';

        for (const auto& broker : fees.mBrokers)
        {
            writeSummaryRow(aOut,
                            std::string{brokerName(broker.mBroker)},
                            broker.mByCurrency,
                            broker.mEurTotal,
                            "fee",
                            kFeeReason);
        }

        writeSummaryTotal(aOut, fees.mBrokers, fees.mEurTotal, kFeeReason);
    }

    aOut << "Benefits in " << year << " (information only, not in any form):";

    if (benefits.mBrokers.empty())
    {
        aOut << " none\n";
    }
    else
    {
        aOut << '\n';

        for (const auto& row : benefits.mBrokers)
        {
            const auto label = std::string{brokerName(row.mBroker)} + ", " +
                               std::string{benefitTypeText(row.mType)};

            writeSummaryRow(aOut, label, row.mByCurrency, row.mEurTotal, "benefit", kBenefitReason);
        }

        writeSummaryTotal(aOut, benefits.mBrokers, benefits.mEurTotal, kBenefitReason);
    }

    aOut << '\n';
}

/// The errors and the warnings of a form. The Doh-KDVP lists are always written; an income form
/// has them only when it has a diagnostic.
void writeFormDiagnostics(std::ostream& aOut, const RunOutcome& aOutcome, const FormInfo& aForm) {
    std::vector<const ApplicationDiagnostic*> errors;
    std::vector<const ApplicationDiagnostic*> warnings;

    for (const auto& diagnostic : aOutcome.mResult.mDiagnostics)
    {
        if (!affects(diagnostic, aForm.mType))
        {
            continue;
        }

        (diagnostic.mSeverity == DiagnosticSeverity::Error ? errors : warnings)
            .push_back(&diagnostic);
    }

    if (aForm.mType != ReportType::CapitalGains && errors.empty() && warnings.empty())
    {
        return;
    }

    const std::string code{aForm.mCode};

    aOut << '\n';
    writeDiagnostics(aOut, code + " errors", errors, false, aForm.mType);
    aOut << '\n';
    writeDiagnostics(aOut, code + " warnings", warnings, false, aForm.mType);
}

/// The years as ranges, such as "2016 to 2024, 2026".
std::string yearRanges(const std::vector<int>& aYears) {
    std::string text;

    for (std::size_t index = 0; index < aYears.size();)
    {
        std::size_t last = index;

        while (last + 1 < aYears.size() && aYears[last + 1] == aYears[last] + 1)
        {
            ++last;
        }

        text += text.empty() ? "" : ", ";
        text += std::to_string(aYears[index]);

        if (last > index)
        {
            text += " to " + std::to_string(aYears[last]);
        }

        index = last + 1;
    }

    return text;
}

/// The "Reference data" line: where each of the three files came from.
std::string referenceDataLine(const ApplicationDataSources& aSources) {
    std::string supplied;
    std::string builtIn;
    std::string revision;

    for (const auto& file : aSources.mReferenceFiles)
    {
        std::string& target = file.mOrigin == CatalogueOrigin::Supplied ? supplied : builtIn;

        target += target.empty() ? "" : ", ";
        target += file.mFile;

        if (!file.mUpstreamRevision.empty())
        {
            // The revision of the upstream data, short like a commit name.
            revision = file.mUpstreamRevision.substr(0, 7);
        }
    }

    if (supplied.empty())
    {
        return "Reference data: built-in" +
               (revision.empty() ? "" : " (ib-edavki " + revision + ")") + "\n";
    }

    return "Reference data: " + supplied + " supplied" +
           (builtIn.empty() ? "" : "; " + builtIn + " built-in") + "\n";
}

/// The "ECB rate files" line: the built-in years in use and the supplied files.
std::string rateFilesLine(const ApplicationDataSources& aSources) {
    std::vector<int> builtInYears;
    std::string supplied;

    for (const auto& file : aSources.mRateFiles)
    {
        if (file.mOrigin == OfficialRateOrigin::BuiltIn)
        {
            builtInYears.push_back(file.mYear);
            continue;
        }

        supplied += supplied.empty() ? "" : ", ";
        supplied += file.mFile + " (" + std::to_string(file.mYear) +
                    (file.mReplacesBuiltIn ? ", replaces the built-in year" : "") + ")";
    }

    std::string line = "ECB rate files: ";

    if (!builtInYears.empty())
    {
        line += "built-in " + yearRanges(builtInYears);
    }

    if (!supplied.empty())
    {
        line += std::string{builtInYears.empty() ? "" : "; "} + "supplied: " + supplied;
    }

    return line + "\n";
}

std::string summaryText(const RunOutcome& aOutcome) {
    std::ostringstream out;
    const auto& request = aOutcome.mRequest;

    out << "TaxBrokerReport " << request.mReportingYear << " ("
        << (request.mFilingType == FilingType::Original ? "original return" : "self-report")
        << ")\n";

    if (request.mFilingType == FilingType::SelfReport)
    {
        out << "The income forms are always original returns.\n";
    }

    out << "Input files: " << request.mBrokerFiles.size() << '\n';

    for (std::size_t index = 0; index < request.mBrokerFiles.size(); ++index)
    {
        out << "  " << sourceId(index) << ' ' << request.mBrokerFiles[index].mFile.mName << '\n';
    }

    out << referenceDataLine(aOutcome.mResult.mDataSources)
        << rateFilesLine(aOutcome.mResult.mDataSources) << '\n';
    writeForms(out, aOutcome);
    out << '\n';
    writeInvestments(out, aOutcome);
    out << '\n';
    writeExcluded(out, aOutcome);
    writeExcludedItems(out, aOutcome.mResult);
    writeLongHeld(out, aOutcome);
    out << '\n';
    writeUsedRates(out, aOutcome);
    writeFeesAndBenefits(out, aOutcome);
    writePrivateMarketEvents(out, aOutcome);

    for (const auto& form : kForms)
    {
        writeFormDiagnostics(out, aOutcome, form);
    }

    writeOpenQuestions(out, aOutcome.mResult);
    writeAnswers(out, aOutcome.mResult);

    std::vector<const ApplicationDiagnostic*> others;

    for (const auto& diagnostic : aOutcome.mResult.mDiagnostics)
    {
        const bool affectsAForm =
            std::any_of(kForms.begin(), kForms.end(), [&diagnostic](const FormInfo& aForm) {
                return affects(diagnostic, aForm.mType);
            });

        if (!affectsAForm)
        {
            others.push_back(&diagnostic);
        }
    }

    if (!others.empty())
    {
        out << '\n';
        writeDiagnostics(out, "Not about a form (skipped private-market rows)", others, true, {});
    }

    return out.str();
}

bool writeFile(const std::filesystem::path& aPath, std::string_view aText) {
    std::ofstream output{aPath, std::ios::binary};

    output << aText;
    output.close();

    return static_cast<bool>(output);
}

/// True when nothing is at the path or a file is there, so the path can be replaced.
bool isFileOrMissing(const std::filesystem::path& aPath) {
    std::error_code error;
    const auto status = std::filesystem::status(aPath, error);

    return !std::filesystem::exists(status) || std::filesystem::is_regular_file(status);
}

/// Asks the open questions, writes the output files and the summary, and returns the exit code.
int runReport(const ReportOptions& aOptions,
              ApplicationRun& aRun,
              std::istream& aIn,
              std::ostream& aOut,
              std::ostream& aErr) {
    if (aOptions.mAskQuestions)
    {
        askOpenQuestions(aRun.session(), aIn, aOut);
    }

    // The run keeps the result of the last calculation that succeeded: a rejected answer
    // calculates nothing.
    const auto& result = aRun.result();

    std::error_code error;
    std::filesystem::create_directories(aOptions.mOutputDirectory, error);

    if (error)
    {
        aErr << "taxbroker: the output folder cannot be created.\n";
        return kExitFailure;
    }

    const auto summaryPath = aOptions.mOutputDirectory / summaryFileName(aOptions.mYear);

    // A folder at an output path would make the run fail after another file was changed, so it
    // is refused before anything is written. A form that is not generated writes nothing, and its
    // file of an earlier run stays as it is.
    std::array<bool, 4> keptOldFile{};
    bool allDone = true;

    for (const auto& form : kForms)
    {
        const auto index = static_cast<std::size_t>(form.mType);
        const auto status = result.report(form.mType).mStatus;
        const auto xmlPath = aOptions.mOutputDirectory / xmlFileName(form, aOptions.mYear);
        const bool generated = status == ReportStatus::Generated;

        allDone = allDone && (generated || status == ReportStatus::NoData);

        if (generated && !isFileOrMissing(xmlPath))
        {
            aErr << "taxbroker: " << xmlFileName(form, aOptions.mYear) << " cannot be written.\n";
            return kExitFailure;
        }

        keptOldFile[index] = !generated && std::filesystem::is_regular_file(xmlPath, error);
    }

    if (!isFileOrMissing(summaryPath))
    {
        aErr << "taxbroker: " << summaryFileName(aOptions.mYear) << " cannot be written.\n";
        return kExitFailure;
    }

    const RunOutcome outcome{
        .mRequest = aRun.request(),
        .mResult = result,
        .mKeptOldFile = keptOldFile,
    };
    const auto summary = summaryText(outcome);

    // Every file is written under a temporary name first, so a run that fails here (exit 2)
    // leaves the files of an earlier run as they were.
    std::vector<FileReplacement> replacements;
    std::vector<std::filesystem::path> temporaries;

    for (const auto& form : kForms)
    {
        const auto& report = result.report(form.mType);

        if (report.mStatus != ReportStatus::Generated)
        {
            continue;
        }

        const auto xmlPath = aOptions.mOutputDirectory / xmlFileName(form, aOptions.mYear);
        const auto xmlTemporary = temporaryPath(xmlPath);

        temporaries.push_back(xmlTemporary);

        if (!writeFile(xmlTemporary, report.mXml.value()))
        {
            removeTemporaryFiles(temporaries);
            aErr << "taxbroker: " << xmlFileName(form, aOptions.mYear) << " cannot be written.\n";
            return kExitFailure;
        }

        replacements.push_back({.mTemporary = xmlTemporary, .mFinal = xmlPath});
    }

    const auto summaryTemporary = temporaryPath(summaryPath);

    temporaries.push_back(summaryTemporary);

    if (!writeFile(summaryTemporary, summary))
    {
        removeTemporaryFiles(temporaries);
        aErr << "taxbroker: " << summaryFileName(aOptions.mYear) << " cannot be written.\n";
        return kExitFailure;
    }

    replacements.push_back({.mTemporary = summaryTemporary, .mFinal = summaryPath});

    // The final paths hold a file or nothing. A failing step puts back the files of an earlier
    // run, so the run changes nothing (exit 2).
    const auto replaced = replaceFiles(replacements);

    if (!replaced.mReplaced)
    {
        aErr << "taxbroker: the output files cannot be replaced";

        if (!replaced.mNotRestored.empty())
        {
            aErr << ", and ";

            for (std::size_t index = 0; index < replaced.mNotRestored.size(); ++index)
            {
                aErr << (index == 0 ? "" : ", ") << replaced.mNotRestored[index];
            }

            aErr << " of an earlier run could not be restored";
        }

        aErr << ".\n";
        return kExitFailure;
    }

    aOut << summary;

    return allDone ? kExitSuccess : kExitBlocked;
}

/// Writes the reason a supplied rate or reference file is not valid: the file name only, never
/// the path.
void writeInputFault(const ApplicationInputFault& aFault, std::ostream& aErr) {
    const auto visitor = [&aErr](const auto& aValue) {
        using Fault = std::decay_t<decltype(aValue)>;

        if constexpr (std::is_same_v<Fault, RateFileFault>)
        {
            const auto& first = aValue.mDiagnostic;

            aErr << "taxbroker: the ECB rate file " << first.mSourceFile << " is not valid";

            if (first.mRowIndex)
            {
                aErr << " (row " << *first.mRowIndex << ")";
            }

            aErr << ": " << first.mMessage << '\n' << kHelpHint;
        }
        else
        {
            static_assert(std::is_same_v<Fault, ReferenceFileFault>);

            // The text of a wrong or repeated file name already names the file.
            const bool nameFault = aValue.mMessage.mKey == MessageKey::ReferenceFileUnknownName ||
                                   aValue.mMessage.mKey == MessageKey::ReferenceFileRepeatedName;

            if (nameFault)
            {
                aErr << "taxbroker: " << aValue.mMessage.mEnglish << '\n' << kHelpHint;

                return;
            }

            aErr << "taxbroker: the reference file " << aValue.mFile << " is not valid";

            if (aValue.mRow)
            {
                aErr << " (row " << *aValue.mRow << ")";
            }

            aErr << ": " << aValue.mMessage.mEnglish << '\n' << kHelpHint;
        }
    };

    std::visit(visitor, aFault);
}

/// Reads each file into memory and hands it to aAdd. On the first file that cannot be read it
/// writes the reason to aErr (the file name only, never the path) and returns false.
template <typename Add>
bool readInputFiles(const std::vector<std::filesystem::path>& aPaths,
                    std::string_view aKind,
                    Add aAdd,
                    std::ostream& aErr) {
    for (std::size_t index = 0; index < aPaths.size(); ++index)
    {
        auto read = readInputFile(aPaths[index]);

        if (const auto* fault = std::get_if<InputFileFault>(&read))
        {
            aErr << "taxbroker: the " << aKind << ' ' << fileNameOf(aPaths[index]);

            if (*fault == InputFileFault::Unreadable)
            {
                aErr << " could not be read.\n" << kHelpHint;
            }
            else
            {
                aErr << " cannot be used: " << inputFileFaultMessage(*fault, aKind, index).mEnglish
                     << '\n'
                     << kHelpHint;
            }

            return false;
        }

        aAdd(std::move(std::get<InputFile>(read)));
    }

    return true;
}

int runReportCommand(std::span<const std::string> aArguments,
                     std::istream& aIn,
                     std::ostream& aOut,
                     std::ostream& aErr) {
    const auto parsed = parseReportArguments(aArguments);

    if (std::holds_alternative<HelpRequested>(parsed))
    {
        aOut << kReportHelp;
        return kExitSuccess;
    }

    if (const auto* usage = std::get_if<UsageError>(&parsed))
    {
        aErr << "taxbroker: " << usage->mMessage << '\n' << kHelpHint;
        return kExitFailure;
    }

    const auto& options = std::get<ReportOptions>(parsed);

    if (const auto problem = checkInputFiles(options))
    {
        aErr << "taxbroker: " << problem->mMessage << '\n' << kHelpHint;
        return kExitFailure;
    }

    const auto taxpayer = readTaxpayerFile(options.mTaxpayerPath);

    if (const auto* problem = std::get_if<TaxpayerFileError>(&taxpayer))
    {
        aErr << "taxbroker: the taxpayer file " << fileNameOf(options.mTaxpayerPath)
             << " is not valid: " << problem->mMessage << '\n'
             << kHelpHint;
        return kExitFailure;
    }

    // Every user file is read into memory once, in the order of the command line. A file that
    // cannot be read fails the run before anything is calculated.
    ApplicationRequest request{
        .mReportingYear = options.mYear,
        .mFilingType = options.mFilingType,
        .mTaxpayer = std::get<TaxpayerMetadata>(taxpayer),
    };

    if (!readInputFiles(
            options.mTradeRepublicPaths,
            "CSV file",
            [&request](InputFile&& aFile) {
                request.mBrokerFiles.push_back(
                    {.mBroker = Broker::TradeRepublic, .mFile = std::move(aFile)});
            },
            aErr) ||
        !readInputFiles(
            options.mEcbRatePaths,
            "ECB rate file",
            [&request](InputFile&& aFile) { request.mEcbRateFiles.push_back(std::move(aFile)); },
            aErr) ||
        !readInputFiles(
            options.mReferencePaths,
            "reference file",
            [&request](InputFile&& aFile) { request.mReferenceFiles.push_back(std::move(aFile)); },
            aErr))
    {
        return kExitFailure;
    }

    // A supplied rate or reference file that is not valid fails the run before anything is
    // calculated.
    auto started = ApplicationRun::start(std::move(request));

    if (const auto* fault = std::get_if<ApplicationInputFault>(&started))
    {
        writeInputFault(*fault, aErr);

        return kExitFailure;
    }

    return runReport(options, std::get<ApplicationRun>(started), aIn, aOut, aErr);
}

} // namespace

int runCli(std::span<const std::string> aArguments,
           std::istream& aIn,
           std::ostream& aOut,
           std::ostream& aErr) {
    if (aArguments.empty())
    {
        aErr << kGeneralHelp;
        return kExitFailure;
    }

    const std::string& command = aArguments.front();

    if (command == "-h" || command == "--help" || command == "help")
    {
        aOut << kGeneralHelp;
        return kExitSuccess;
    }

    if (command != "report")
    {
        // Only the last part, so a CSV path given without `report` prints no folder.
        aErr << "taxbroker: unknown command " << fileNameOf(command) << ".\n"
             << "Run 'taxbroker --help' for the commands.\n";
        return kExitFailure;
    }

    try
    {
        return runReportCommand(aArguments.subspan(1), aIn, aOut, aErr);
    } catch (const std::exception& exception)
    {
        aErr << "taxbroker: the report failed unexpectedly: " << exception.what() << '\n';
        return kExitFailure;
    }
}

} // namespace taxbroker::cli
