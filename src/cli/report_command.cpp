#include "cli/report_command.hpp"

#include "cli/decision_prompts.hpp"
#include "cli/taxpayer_file.hpp"
#include "cli_text.hpp"
#include "parsers/traderepublic_parser.hpp"
#include "processors/capital_failure_resolution.hpp"
#include "processors/capital_gains_report_builder.hpp"
#include "processors/capital_report_gates.hpp"
#include "processors/corporate_actions.hpp"
#include "processors/fifo_matcher.hpp"
#include "processors/loss_eligibility.hpp"
#include "processors/tax_processor.hpp"
#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"
#include "xml/kdvp_xml_writer.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <istream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
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
                   --out DIR CSV [CSV ...]
  taxbroker report --help
  taxbroker --help

Commands:
  report    Write the Doh-KDVP form (capital gains) for one year.

Run 'taxbroker report --help' for the details.
)";

constexpr std::string_view kReportHelp =
    R"(Usage:
  taxbroker report --year YYYY --taxpayer FILE [--self-report] [--no-input]
                   --out DIR CSV [CSV ...]

Writes the Doh-KDVP form (capital gains from selling shares and funds) for one year from
Trade Republic CSV exports, and a readable summary. The XML can be imported into eDavki
as a draft. Nothing is sent anywhere: everything stays on this computer.

Options:
  --year YYYY        The tax year, 2000 to 9999.
  --taxpayer FILE    Your details as JSON (see "Taxpayer file" below).
  --self-report      Write a self-report (samoprijava) instead of an original return.
  --no-input         Ask no questions: an open question blocks the report (see
                     "Questions" below).
  --out DIR          The folder for the output files. It is created if it is missing.
  -h, --help         Show this help.
  CSV                One or more Trade Republic exports. Give your whole history, not
                     only the tax year: FIFO needs the older purchases. Overlapping files
                     are fine; a transaction in two files is counted once.

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
  Doh-KDVP_YYYY.xml  The form. Written only when it could be made in full.
  summary_YYYY.txt   Each investment sold in the year, the units past the holding period,
                     the skipped private-market events, the warnings, the errors, the
                     open questions and your answers. The same summary is printed here.
  A blocked run, or a year without a sale to report, deletes an old Doh-KDVP_YYYY.xml
  in DIR, so a file from an earlier run is never mistaken for this run's result.

Supported for now:
  - Shares and funds (ETFs) bought and sold in EUR.
  - Units held longer than the holding period (15 years for a sale from 2022, 20 years
    before) are tax free: they are left out of the XML and listed in the summary.
  - The 30-day loss rule: a sale at a loss whose loss a purchase of the same investment
    within 30 days before or after it blocks in full is written with F10 false. A loss
    that the files alone cannot decide is written as you answer (see "Questions").

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
  - Investments you could leave out: when each problem that blocks the report is tied to a
    single investment sold in the year (one or several, for example earlier purchases are
    missing), the command asks one question that lists them all. exclude: write Doh-KDVP
    without them; they are not in the XML and the summary lists them. fail: write no
    Doh-KDVP. A problem with a file, a row, the year or the XML is not solved this way. It
    comes after the questions of other investments and before those of the listed ones,
    which exclude drops.
  An answer that does not fit is explained and the question is asked again. cancel closes
  a question without an answer, and the report is blocked. With --no-input, or when the
  input ends (Ctrl-D), the open questions stay open: the report is blocked and the summary
  lists them. The answers are used only in this run and are never saved, so the next run
  asks again.

The report is blocked (no XML) when an investment sold in the year:
  - is crypto, a private fund, a bond or a derivative, or has an unknown asset class;
  - has a split or reverse split up to 31 December whose ratio is not answered, a merger,
    or split rows of two brokers at most 10 days apart that are not one split. An answered
    split is applied;
  - misses earlier purchase history;
  - has a corporate action without an answered ratio, or a merger, within 30 days after a
    sale at a loss in December;
  - has a split or reverse split that makes the value of one unit too large for the form;
  - has a trade that is not in EUR;
  - has a question that is open or was cancelled.
It is also blocked when a file cannot be read, when a trade or corporate-action row of
any file is rejected, when two files disagree about the same trade or about the asset
class of an investment, or when a value does not fit the form (for example a postCode
that is too long).

Skipped:
  - Private-market events (Trade Republic types PRIVATE_MARKET_BUY, PRIVATE_MARKET_SELL
    and BONUS) have only a cash amount, without units or a price, so they are not
    processed. Each one in the year gets a warning in the summary: check whether you
    have to report it yourself. A private-market row that cannot be read is listed with
    its error, but it does not block the report.
  - Dividends and interest: their forms (Doh-Div, Doh-DHO, Doh-Obr) are not made yet.

Exit codes:
  0  The XML was written, or no sale in the year needs a Doh-KDVP form.
  1  The report is blocked: the errors and the open questions are in the summary. No XML
     was written.
  2  The command could not run: a wrong option, a missing file, an invalid taxpayer
     file or an output file that cannot be written. No output file was changed.

Example:
  taxbroker report --year 2025 --taxpayer taxpayer.json --out out export.csv
)";

constexpr std::string_view kHelpHint = "Run 'taxbroker report --help' for the usage.\n";

/// What the command line of `report` asks for.
struct ReportOptions {
    int mYear{};
    std::filesystem::path mTaxpayerPath;
    FilingType mFilingType{FilingType::Original};
    /// False with --no-input: the open questions stay open.
    bool mAskQuestions{true};
    std::filesystem::path mOutputDirectory;
    std::vector<std::filesystem::path> mCsvPaths;
};

/// A problem that stops the run before anything is written, with its text for stderr.
struct UsageError {
    std::string mMessage;
};

struct HelpRequested {};

/// Only the last part of a path, so no folder of the user is printed.
std::string fileNameOf(const std::filesystem::path& aPath) {
    return aPath.filename().string();
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

/// Reads the options of `report`. Options and CSV paths may come in any order; `--` ends the
/// options.
std::variant<ReportOptions, UsageError, HelpRequested>
parseReportArguments(std::span<const std::string> aArguments) {
    ReportOptions options;
    std::optional<std::string> yearText;
    std::optional<std::string> taxpayerText;
    std::optional<std::string> outputText;
    bool selfReport = false;
    bool noInput = false;
    bool onlyPaths = false;

    for (std::size_t index = 0; index < aArguments.size(); ++index)
    {
        const std::string& argument = aArguments[index];

        if (onlyPaths || argument.empty() || argument.front() != '-')
        {
            options.mCsvPaths.emplace_back(argument);
            continue;
        }

        if (argument == "--")
        {
            onlyPaths = true;
            continue;
        }

        if (argument == "-h" || argument == "--help")
        {
            return HelpRequested{};
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

    if (options.mCsvPaths.empty())
    {
        return UsageError{"Give at least one Trade Republic CSV file."};
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
    const auto missingCsv = std::find_if(aOptions.mCsvPaths.begin(),
                                         aOptions.mCsvPaths.end(),
                                         [&error](const std::filesystem::path& aPath) {
                                             return !std::filesystem::is_regular_file(aPath, error);
                                         });

    if (missingCsv != aOptions.mCsvPaths.end())
    {
        return UsageError{"The CSV file " + fileNameOf(*missingCsv) +
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

std::string sourceText(const SourceReference& aSource) {
    return aSource.mFilename.value() + " row " + std::to_string(aSource.mSourceRow);
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

const TradeTransaction& tradeOf(const MergedStatement& aStatement,
                                const StatementEventReference& aEvent) {
    return aStatement.mPresentation.mTradeInstruments.at(aEvent.mInstrumentIndex.value())
        .mTransactions.at(aEvent.mEventIndex);
}

std::string saleLine(const MergedStatement& aStatement, const StatementEventReference& aSale) {
    const auto& metadata = tradeOf(aStatement, aSale).mMetadata;

    return "sale " + isoDate(metadata.mTaxDate) + ", " + sourceText(primarySource(metadata));
}

bool affectsCapital(const ApplicationDiagnostic& aDiagnostic) {
    return std::find(aDiagnostic.mAffectedReports.begin(),
                     aDiagnostic.mAffectedReports.end(),
                     ReportType::CapitalGains) != aDiagnostic.mAffectedReports.end();
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
/// only for a list that mixes errors and warnings.
void writeDiagnostic(std::ostream& aOut,
                     const ApplicationDiagnostic& aDiagnostic,
                     bool aWithSeverity) {
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

    // An error that does not block was resolved by the user leaving its investment out.
    if (aDiagnostic.mSeverity == DiagnosticSeverity::Error && !aDiagnostic.mBlocking &&
        affectsCapital(aDiagnostic))
    {
        aOut << "    Resolved by your answer: the investment is left out of the XML.\n";
    }
}

void writeDiagnostics(std::ostream& aOut,
                      std::string_view aTitle,
                      const std::vector<const ApplicationDiagnostic*>& aDiagnostics,
                      bool aWithSeverity) {
    aOut << aTitle << ": " << aDiagnostics.size() << '\n';

    for (const auto* diagnostic : aDiagnostics)
    {
        writeDiagnostic(aOut, *diagnostic, aWithSeverity);
    }
}

/// What the summary says about one ISIN with a sale in the selected year.
struct InvestmentLine {
    std::string mName;
    /// Sales and their units, when the ISIN has a list in the filing rows. A sale whose loss is
    /// blocked in part has several rows but is one sale.
    std::size_t mListedSales{};
    Units mListedUnits{};
    Units mBlockedLossUnits{};
    bool mListed{};
    /// The sales the summary names under a blocked ISIN, in ledger order.
    std::vector<StatementEventReference> mSales;
};

/// Everything the summary needs from one run.
struct RunOutcome {
    const ReportOptions& mOptions;
    const MergedStatement& mStatement;
    const CapitalGainsBuildResult& mBuilt;
    /// The filing rows of the ISINs without a problem, also when the report is blocked.
    const CapitalGainsReportKDVP& mPreview;
    const ApplicationResult& mResult;
    bool mDeletedOldXml{};
};

std::string xmlFileName(int aYear) {
    return "Doh-KDVP_" + std::to_string(aYear) + ".xml";
}

std::string summaryFileName(int aYear) {
    return "summary_" + std::to_string(aYear) + ".txt";
}

/// The ISINs with a sale in the selected year: the listed ones, the blocked ones and those with
/// only units past the holding period, by ISIN.
std::map<Isin, InvestmentLine> investmentLines(const RunOutcome& aOutcome) {
    std::map<Isin, InvestmentLine> lines;

    for (const auto& list : aOutcome.mPreview.mLists)
    {
        auto& line = lines[list.mIsin];
        line.mName = list.mName;
        line.mListed = true;

        for (const auto& row : list.mRows)
        {
            if (const auto* sale = std::get_if<KdvpSaleRow>(&row.mEntry))
            {
                // The rows of one sale follow each other.
                if (line.mSales.empty() || line.mSales.back() != sale->mSale)
                {
                    ++line.mListedSales;
                    line.mSales.push_back(sale->mSale);
                }

                line.mListedUnits += sale->mUnits;
                line.mBlockedLossUnits +=
                    sale->mLossRule == KdvpLossRule::Blocked ? sale->mUnits : Units{0};
            }
        }
    }

    for (const auto& longHeld : aOutcome.mBuilt.mLongHeld)
    {
        auto& line = lines[longHeld.mIsin];
        line.mName = longHeld.mName;
        line.mSales.push_back(longHeld.mSale);
    }

    for (const auto& blocked : aOutcome.mBuilt.mBlocked)
    {
        auto& line = lines[blocked.mIsin];
        line.mName = blocked.mName;
        line.mSales = blocked.mSales;
    }

    for (const auto& excluded : aOutcome.mBuilt.mExcluded)
    {
        auto& line = lines[excluded.mIsin];
        line.mName = excluded.mName;
        line.mSales = excluded.mSales;
    }

    for (auto& [isin, line] : lines)
    {
        std::sort(line.mSales.begin(),
                  line.mSales.end(),
                  [](const StatementEventReference& aLeft, const StatementEventReference& aRight) {
                      return aLeft.mEventIndex < aRight.mEventIndex;
                  });
        line.mSales.erase(std::unique(line.mSales.begin(), line.mSales.end()), line.mSales.end());
    }

    return lines;
}

/// The codes of the blocking capital errors of an ISIN, in diagnostic order, each once.
std::vector<std::string_view> blockingCodes(const ApplicationResult& aResult, const Isin& aIsin) {
    std::vector<std::string_view> codes;

    for (const auto& diagnostic : aResult.mDiagnostics)
    {
        if (!diagnostic.mBlocking || !affectsCapital(diagnostic) || diagnostic.mIsin != aIsin)
        {
            continue;
        }

        const auto code = api::diagnosticCodeName(diagnostic.mCode);

        if (std::find(codes.begin(), codes.end(), code) == codes.end())
        {
            codes.push_back(code);
        }
    }

    return codes;
}

/// The excluded investments of the capital result, empty unless the user chose to leave out.
const std::vector<RequestInvestment>& excludedInvestments(const ApplicationResult& aResult) {
    return aResult.report(ReportType::CapitalGains).mExcludedInvestments;
}

void writeCapitalStatus(std::ostream& aOut, const RunOutcome& aOutcome) {
    const int year = aOutcome.mOptions.mYear;

    aOut << "Capital gains (Doh-KDVP): ";

    switch (aOutcome.mResult.report(ReportType::CapitalGains).mStatus)
    {
    case ReportStatus::Generated: {
        const auto excluded = excludedInvestments(aOutcome.mResult).size();

        aOut << "generated"
             << (excluded == 0
                     ? ""
                     : " without " +
                           countText(excluded, "excluded investment", "excluded investments"))
             << ", written to " << xmlFileName(year) << '\n';
        break;
    }
    case ReportStatus::NoData:
        if (!excludedInvestments(aOutcome.mResult).empty())
        {
            aOut << "no data, nothing is left to report after the excluded investments\n";
            break;
        }

        aOut << "no data, no sale in " << year << " needs a Doh-KDVP form\n";
        break;
    case ReportStatus::NeedsInput:
    case ReportStatus::Failed:
        aOut << "blocked, no XML was written (see the errors below)\n";
        break;
    }

    if (aOutcome.mDeletedOldXml)
    {
        aOut << "Deleted the old " << xmlFileName(year) << " of an earlier run.\n";
    }
}

void writeInvestments(std::ostream& aOut, const RunOutcome& aOutcome) {
    const auto lines = investmentLines(aOutcome);
    const bool generated =
        aOutcome.mResult.report(ReportType::CapitalGains).mStatus == ReportStatus::Generated;

    aOut << "Investments with a sale in " << aOutcome.mOptions.mYear << ": " << lines.size()
         << '\n';

    for (const auto& [isin, line] : lines)
    {
        const auto codes = blockingCodes(aOutcome.mResult, isin);
        const auto& excluded = excludedInvestments(aOutcome.mResult);
        const auto left = std::find_if(
            excluded.begin(),
            excluded.end(),
            [&isin](const RequestInvestment& aInvestment) { return aInvestment.mIsin == isin; });

        aOut << "  " << investmentText(isin, line.mName) << ": ";

        if (left != excluded.end())
        {
            aOut << "excluded (" << reasonCodesText(*left) << ")\n";

            for (const auto& sale : line.mSales)
            {
                aOut << "    " << saleLine(aOutcome.mStatement, sale) << '\n';
            }

            continue;
        }

        if (!codes.empty())
        {
            aOut << "blocked (";

            for (std::size_t index = 0; index < codes.size(); ++index)
            {
                aOut << (index == 0 ? "" : ", ") << codes[index];
            }

            aOut << ")\n";

            for (const auto& sale : line.mSales)
            {
                aOut << "    " << saleLine(aOutcome.mStatement, sale) << '\n';
            }

            continue;
        }

        if (!line.mListed)
        {
            aOut << "only units past the holding period, not in the XML\n";
            continue;
        }

        aOut << (generated ? "listed, " : "no problem, ")
             << countText(line.mListedSales, "sale", "sales") << ", "
             << unitsText(line.mListedUnits) << " units";

        if (line.mBlockedLossUnits > 0)
        {
            aOut << ", " << unitsText(line.mBlockedLossUnits)
                 << " units with a loss blocked by the 30-day rule (F10 false)";
        }

        aOut << '\n';
    }
}

/// The investments the user left out, only when there is one.
void writeExcluded(std::ostream& aOut, const RunOutcome& aOutcome) {
    const auto& excluded = excludedInvestments(aOutcome.mResult);

    if (excluded.empty())
    {
        return;
    }

    aOut << "Excluded investments (not in the XML): " << excluded.size() << '\n';

    for (const auto& investment : excluded)
    {
        aOut << "  " << investmentText(investment.mIsin, investment.mName) << ": "
             << reasonCodesText(investment) << '\n';
    }

    aOut << '\n';
}

void writeLongHeld(std::ostream& aOut, const RunOutcome& aOutcome) {
    aOut << "Units past the holding period (tax free, not in the XML): "
         << aOutcome.mBuilt.mLongHeld.size() << '\n';

    for (const auto& longHeld : aOutcome.mBuilt.mLongHeld)
    {
        const auto& metadata = tradeOf(aOutcome.mStatement, longHeld.mSale).mMetadata;

        aOut << "  " << investmentText(longHeld.mIsin, longHeld.mName) << ": sale "
             << isoDate(metadata.mTaxDate) << ", " << unitsText(longHeld.mUnits) << " units\n";
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
            return countText(aQuestion.mInvestments.size(), "investment", "investments");
        }
        else if constexpr (std::is_same_v<Question, LossEligibilityQuestion>)
        {
            return aQuestion.mName;
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

std::string summaryText(const RunOutcome& aOutcome) {
    std::ostringstream out;
    const auto& options = aOutcome.mOptions;

    out << "TaxBrokerReport: Doh-KDVP " << options.mYear << " ("
        << (options.mFilingType == FilingType::Original ? "original return" : "self-report")
        << ")\nInput files: " << options.mCsvPaths.size() << '\n';

    for (std::size_t index = 0; index < options.mCsvPaths.size(); ++index)
    {
        out << "  " << sourceId(index) << ' ' << fileNameOf(options.mCsvPaths[index]) << '\n';
    }

    out << '\n';
    writeCapitalStatus(out, aOutcome);
    out << '\n';
    writeInvestments(out, aOutcome);
    out << '\n';
    writeExcluded(out, aOutcome);
    writeLongHeld(out, aOutcome);
    out << '\n';
    writePrivateMarketEvents(out, aOutcome);

    std::vector<const ApplicationDiagnostic*> errors;
    std::vector<const ApplicationDiagnostic*> warnings;
    std::vector<const ApplicationDiagnostic*> others;

    for (const auto& diagnostic : aOutcome.mResult.mDiagnostics)
    {
        if (!affectsCapital(diagnostic))
        {
            others.push_back(&diagnostic);
        }
        else if (diagnostic.mSeverity == DiagnosticSeverity::Error)
        {
            errors.push_back(&diagnostic);
        }
        else
        {
            warnings.push_back(&diagnostic);
        }
    }

    out << '\n';
    writeDiagnostics(out, "Errors", errors, false);
    out << '\n';
    writeDiagnostics(out, "Warnings", warnings, false);
    writeOpenQuestions(out, aOutcome.mResult);
    writeAnswers(out, aOutcome.mResult);

    if (!others.empty())
    {
        out << '\n';
        writeDiagnostics(out,
                         "Not about Doh-KDVP (dividends, interest and skipped private-market rows)",
                         others,
                         true);
    }

    return out.str();
}

bool writeFile(const std::filesystem::path& aPath, std::string_view aText) {
    std::ofstream output{aPath, std::ios::binary};

    output << aText;
    output.close();

    return static_cast<bool>(output);
}

/// True when nothing is at the path or a file is there, so the path can be replaced or deleted.
bool isFileOrMissing(const std::filesystem::path& aPath) {
    std::error_code error;
    const auto status = std::filesystem::status(aPath, error);

    return !std::filesystem::exists(status) || std::filesystem::is_regular_file(status);
}

/// The name an output file is written under before it replaces the file of an earlier run.
std::filesystem::path temporaryPath(const std::filesystem::path& aPath) {
    auto path = aPath;
    path += ".tmp";

    return path;
}

/// Deletes the temporary files of a failed run. Something that is not a file is left alone: this
/// run did not make it.
void removeTemporaryFiles(std::initializer_list<std::filesystem::path> aPaths) {
    for (const auto& path : aPaths)
    {
        std::error_code error;

        if (std::filesystem::is_regular_file(path, error))
        {
            std::filesystem::remove(path, error);
        }
    }
}

/// The ISINs that the builder blocked, for a preview of the rows of every other ISIN.
std::vector<Isin> blockedIsins(const CapitalGainsBuildResult& aBuilt) {
    std::vector<Isin> isins;

    std::transform(aBuilt.mBlocked.begin(),
                   aBuilt.mBlocked.end(),
                   std::back_inserter(isins),
                   [](const KdvpBlockedIsin& aBlocked) { return aBlocked.mIsin; });

    return isins;
}

/// The stage results of a calculation that the summary needs beside the application result.
struct CalculatedRun {
    CapitalGainsBuildResult mBuilt;
    /// The rows of the ISINs without a problem, when the builder made no report.
    std::optional<CapitalGainsReportKDVP> mPreviewWithoutBlocked;
};

/// One calculation from the merged files and the answers: FIFO, the corporate-action stage, the
/// loss rule, the builder, the gates, the choice to leave failed investments out and the XML. It
/// reads nothing from aCalculated, so it keeps no state between calls; aCalculated only receives
/// what the summary needs.
ApplicationResultInput calculateReport(const ReportOptions& aOptions,
                                       const TaxpayerMetadata& aTaxpayer,
                                       const TaxProcessingResult& aPrepared,
                                       RunDecisions& aDecisions,
                                       std::optional<CalculatedRun>& aCalculated) {
    // The year was checked with the options, so the ledger is always there.
    const auto& ledger = aPrepared.mLedger.value();
    const auto& statement = aPrepared.mMerged.mStatement;
    auto pools = matchCapitalPools(statement, ledger, aDecisions);
    auto& fifo = pools.mFifo;
    auto& actions = pools.mActions;
    auto loss = LossEligibilityChecker::check(statement, ledger, fifo, aDecisions);
    auto built = CapitalGainsReportBuilder::build(statement, ledger, fifo, loss, {});
    auto gateDiagnostics = CapitalReportGates::check(statement, ledger, fifo, loss, built);

    // Copied, not moved: every calculation of the run starts from the same diagnostics.
    ApplicationResultInput input{
        .mReportingYear = aOptions.mYear,
        .mMergeDiagnostics = aPrepared.mMerged.mDiagnostics,
        .mProcessingDiagnostics = aPrepared.mDiagnostics,
    };
    auto& processing = input.mProcessingDiagnostics;

    std::move(fifo.mDiagnostics.begin(), fifo.mDiagnostics.end(), std::back_inserter(processing));
    appendStageOutput(input, std::move(actions.mDiagnostics), std::move(actions.mRequests));
    appendStageOutput(input, std::move(loss.mDiagnostics), std::move(loss.mRequests));

    const auto firstGateDiagnostic = processing.size();

    std::move(gateDiagnostics.begin(), gateDiagnostics.end(), std::back_inserter(processing));

    // The user may leave out every investment with an error of its own (or ask for no report).
    const auto excluded = CapitalFailureResolution::resolve(input, aDecisions);

    if (!excluded.empty())
    {
        built = CapitalGainsReportBuilder::build(statement, ledger, fifo, loss, excluded);
    }

    std::optional<CapitalGainsReportKDVP> previewWithoutBlocked;

    if (!built.mReport)
    {
        auto leftOut = excluded;
        const auto blocked = blockedIsins(built);

        leftOut.insert(leftOut.end(), blocked.begin(), blocked.end());
        previewWithoutBlocked =
            CapitalGainsReportBuilder::build(statement, ledger, fifo, loss, leftOut).mReport;
    }

    // A gate error of an excluded investment was resolved and does not stop the XML.
    bool hasGateError = false;

    for (auto position = firstGateDiagnostic; position < processing.size(); ++position)
    {
        const bool resolved = std::find(input.mResolvedDiagnostics.begin(),
                                        input.mResolvedDiagnostics.end(),
                                        position) != input.mResolvedDiagnostics.end();

        hasGateError = hasGateError ||
                       (processing[position].mSeverity == DiagnosticSeverity::Error && !resolved);
    }

    // A blocked ISIN always has an error (FIFO, corporate action or gate), so without a report
    // the capital form is failed or needs input, never empty.
    if (built.mReport && !hasGateError)
    {
        auto& capital = input.mReports[static_cast<std::size_t>(ReportType::CapitalGains)];

        if (built.mReport->mLists.empty())
        {
            // When investments were left out, nothing is left to report, which is not the same
            // as having no sale.
            if (excluded.empty())
            {
                capital.mNoDataReason = NoDataReason::NoSelectedYearDisposals;
            }
        }
        else
        {
            const ReportMetadata metadata{
                .mTaxpayer = aTaxpayer,
                .mReportingYear = aOptions.mYear,
                .mFilingType = aOptions.mFilingType,
            };
            auto written = KdvpXmlWriter::write(metadata, *built.mReport);

            std::move(written.mDiagnostics.begin(),
                      written.mDiagnostics.end(),
                      std::back_inserter(processing));
            capital.mXml = std::move(written.mXml);
        }
    }

    aCalculated = CalculatedRun{
        .mBuilt = std::move(built),
        .mPreviewWithoutBlocked = std::move(previewWithoutBlocked),
    };

    return input;
}

/// Parses and merges the files, calculates the report in a decision session, asks the open
/// questions, writes the output files and the summary, and returns the exit code.
int runReport(const ReportOptions& aOptions,
              const TaxpayerMetadata& aTaxpayer,
              std::istream& aIn,
              std::ostream& aOut,
              std::ostream& aErr) {
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs;

    for (std::size_t index = 0; index < aOptions.mCsvPaths.size(); ++index)
    {
        inputs.push_back({index, parser.parse(aOptions.mCsvPaths[index], index)});
    }

    // Parsing, merging and the year do not depend on an answer, so they run once.
    const auto prepared = TaxProcessor::prepareReportingYear(TaxProcessingRequest{
        .mReportingYear = aOptions.mYear,
        .mMerged = DeterministicStatementMerger{}.merge(std::move(inputs)),
    });
    const auto& statement = prepared.mMerged.mStatement;
    std::optional<CalculatedRun> calculated;
    // The answers live only in this session, in memory, and end with this run.
    DecisionSession session;

    session.start([&](RunDecisions& aDecisions) {
        return calculateReport(aOptions, aTaxpayer, prepared, aDecisions, calculated);
    });

    if (aOptions.mAskQuestions)
    {
        askOpenQuestions(session, aIn, aOut);
    }

    // The session keeps the result of the last calculation that succeeded, and that calculation
    // filled calculated: a rejected answer calculates nothing.
    const auto& result = session.result();
    const auto& built = calculated.value().mBuilt;
    const auto& preview =
        built.mReport ? *built.mReport : calculated.value().mPreviewWithoutBlocked.value();
    const auto& capitalResult = result.report(ReportType::CapitalGains);

    std::error_code error;
    std::filesystem::create_directories(aOptions.mOutputDirectory, error);

    if (error)
    {
        aErr << "taxbroker: the output folder cannot be created.\n";
        return kExitFailure;
    }

    const auto xmlPath = aOptions.mOutputDirectory / xmlFileName(aOptions.mYear);
    const auto summaryPath = aOptions.mOutputDirectory / summaryFileName(aOptions.mYear);
    const bool generated = capitalResult.mStatus == ReportStatus::Generated;

    // A folder at an output path would make the run fail after the other file was changed, so
    // it is refused before anything is written.
    if (!isFileOrMissing(xmlPath) && generated)
    {
        aErr << "taxbroker: " << xmlFileName(aOptions.mYear) << " cannot be written.\n";
        return kExitFailure;
    }

    if (!isFileOrMissing(xmlPath))
    {
        aErr << "taxbroker: the old " << xmlFileName(aOptions.mYear) << " cannot be deleted.\n";
        return kExitFailure;
    }

    if (!isFileOrMissing(summaryPath))
    {
        aErr << "taxbroker: " << summaryFileName(aOptions.mYear) << " cannot be written.\n";
        return kExitFailure;
    }

    const bool deletesOldXml = !generated && std::filesystem::exists(xmlPath, error);
    const RunOutcome outcome{
        .mOptions = aOptions,
        .mStatement = statement,
        .mBuilt = built,
        .mPreview = preview,
        .mResult = result,
        .mDeletedOldXml = deletesOldXml,
    };
    const auto summary = summaryText(outcome);

    // Both files are written under temporary names first, so a run that fails here (exit 2)
    // leaves the files of an earlier run as they were.
    const auto xmlTemporary = temporaryPath(xmlPath);
    const auto summaryTemporary = temporaryPath(summaryPath);

    if (generated && !writeFile(xmlTemporary, capitalResult.mXml.value()))
    {
        removeTemporaryFiles({xmlTemporary});
        aErr << "taxbroker: " << xmlFileName(aOptions.mYear) << " cannot be written.\n";
        return kExitFailure;
    }

    if (!writeFile(summaryTemporary, summary))
    {
        removeTemporaryFiles({xmlTemporary, summaryTemporary});
        aErr << "taxbroker: " << summaryFileName(aOptions.mYear) << " cannot be written.\n";
        return kExitFailure;
    }

    // The final paths hold a file or nothing, and the temporary files are in the same folder, so
    // these steps only rename or delete inside a folder that was just written to.
    std::error_code replaceError;

    if (generated)
    {
        std::filesystem::rename(xmlTemporary, xmlPath, replaceError);
    }
    else if (deletesOldXml)
    {
        std::filesystem::remove(xmlPath, replaceError);
    }

    if (!replaceError)
    {
        std::filesystem::rename(summaryTemporary, summaryPath, replaceError);
    }

    if (replaceError)
    {
        removeTemporaryFiles({xmlTemporary, summaryTemporary});
        aErr << "taxbroker: the output files cannot be replaced.\n";
        return kExitFailure;
    }

    aOut << summary;

    if (generated || capitalResult.mStatus == ReportStatus::NoData)
    {
        return kExitSuccess;
    }

    return kExitBlocked;
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

    return runReport(options, std::get<TaxpayerMetadata>(taxpayer), aIn, aOut, aErr);
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
