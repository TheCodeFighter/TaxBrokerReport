#include "taxbroker/official_rates.hpp"

#include "taxbroker/currency.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "utils/csv_text_reader.hpp"
#include "utils/date_utils.hpp"
#include "utils/numeric_util.hpp"
#include "utils/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <variant>

namespace taxbroker {

namespace {

constexpr std::array<std::string_view, 9> kRequiredColumns{
    "KEY",
    "FREQ",
    "CURRENCY",
    "CURRENCY_DENOM",
    "EXR_TYPE",
    "EXR_SUFFIX",
    "TIME_PERIOD",
    "OBS_VALUE",
    "OBS_STATUS",
};

/// A column that must hold the same value in every row of an ECB daily reference-rate file.
struct FixedColumn {
    std::string_view mName;
    std::string_view mExpected;
};

/// `1 EUR = OBS_VALUE CURRENCY` is the daily (D), spot (SP00), average (A) series against EUR.
constexpr std::array<FixedColumn, 4> kFixedColumns{
    FixedColumn{"FREQ", "D"},
    FixedColumn{"CURRENCY_DENOM", "EUR"},
    FixedColumn{"EXR_TYPE", "SP00"},
    FixedColumn{"EXR_SUFFIX", "A"},
};

/// The only OBS_STATUS accepted: the ECB code for a normal value.
constexpr std::string_view kNormalStatus = "A";

/// The rates read from one accepted file.
struct ParsedRateFile {
    OfficialRateFile mInfo;
    std::vector<OfficialRate> mRates;
};

ParseDiagnostic rateDiagnostic(const SourceFilename& aSourceFile,
                               DiagnosticCode aCode,
                               DiagnosticMessage aMessage,
                               std::optional<std::size_t> aRow = std::nullopt,
                               std::optional<std::string> aField = std::nullopt) {
    return ParseDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = aCode,
        .mSourceFile = aSourceFile.value(),
        .mRowIndex = aRow,
        .mField = std::move(aField),
        .mMessage = std::move(aMessage.mEnglish),
        .mMessageKey = aMessage.mKey,
        .mMessageParams = std::move(aMessage.mParams),
    };
}

SourceFilename displayName(const std::filesystem::path& aPath) {
    try
    { return SourceFilename::fromPath(aPath.string()); } catch (const std::invalid_argument&)
    { return SourceFilename{}; }
}

bool isCurrencyCode(std::string_view aValue) {
    return aValue.size() == 3 && aValue != "EUR" &&
           std::all_of(aValue.begin(), aValue.end(), [](char aCharacter) {
               return aCharacter >= 'A' && aCharacter <= 'Z';
           });
}

/// Maps the first fault of the reader to the rate-file diagnostic.
ParseDiagnostic faultDiagnostic(const SourceFilename& aSourceFile, const CsvFault& aFault) {
    switch (aFault.mKind)
    {
    case CsvFaultKind::UnclosedQuote:
    case CsvFaultKind::LineBreakInField:
        return rateDiagnostic(aSourceFile,
                              DiagnosticCode::ParseError,
                              makeDiagnosticMessage(MessageKey::ParseErrorUnclosedQuote),
                              aFault.mRowNumber);
    case CsvFaultKind::MissingColumns: {
        std::string columns;

        for (const std::string& column : aFault.mColumns)
        {
            columns += columns.empty() ? "" : ", ";
            columns += column;
        }

        return rateDiagnostic(aSourceFile,
                              DiagnosticCode::InvalidHeader,
                              makeDiagnosticMessage(MessageKey::InvalidHeaderRateFileColumns,
                                                    {numberParam("count", aFault.mColumns.size()),
                                                     textParam("columns", columns)}));
    }
    case CsvFaultKind::RepeatedColumn:
        return rateDiagnostic(
            aSourceFile,
            DiagnosticCode::InvalidHeader,
            makeDiagnosticMessage(MessageKey::InvalidHeaderRateFileDuplicateColumn,
                                  {textParam("column", aFault.mColumns.front())}));
    case CsvFaultKind::Unreadable:
        return rateDiagnostic(aSourceFile,
                              DiagnosticCode::ParseError,
                              makeDiagnosticMessage(MessageKey::ParseErrorUnreadableRateFile));
    case CsvFaultKind::FieldCountMismatch:
        return rateDiagnostic(
            aSourceFile,
            DiagnosticCode::FieldCountMismatch,
            makeDiagnosticMessage(MessageKey::FieldCountMismatchRateFile,
                                  {numberParam("expected", aFault.mExpectedFields),
                                   numberParam("found", aFault.mFoundFields)}),
            aFault.mRowNumber);
    }

    throw std::logic_error{"Unknown CSV fault kind"};
}

/// Reads one file into aParsed and returns no diagnostic, or returns the first fault. The checks
/// are the rate-file rules in `calculations.md`.
std::optional<ParseDiagnostic> readRateFile(const std::filesystem::path& aPath,
                                            const SourceFilename& aSourceFile,
                                            ParsedRateFile& aParsed) {
    std::ifstream input{aPath, std::ios::binary};

    if (!input)
    {
        return rateDiagnostic(aSourceFile,
                              DiagnosticCode::ParseError,
                              makeDiagnosticMessage(MessageKey::ParseErrorUnreadableRateFile));
    }

    const std::string content{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    const CsvTable table = readCsvText(content, ',', kRequiredColumns);

    // These faults come before any row; the others stop the rows and are mapped after the loop.
    const bool headerFault = table.mFault && (table.mFault->mKind == CsvFaultKind::UnclosedQuote ||
                                              table.mFault->mKind == CsvFaultKind::MissingColumns ||
                                              table.mFault->mKind == CsvFaultKind::RepeatedColumn);

    if (headerFault)
    {
        return faultDiagnostic(aSourceFile, *table.mFault);
    }

    const std::size_t keyColumn = table.columnIndex("KEY");
    const std::size_t currencyColumn = table.columnIndex("CURRENCY");
    const std::size_t dateColumn = table.columnIndex("TIME_PERIOD");
    const std::size_t valueColumn = table.columnIndex("OBS_VALUE");
    const std::size_t statusColumn = table.columnIndex("OBS_STATUS");

    // The index of each fixed column, in the order of kFixedColumns.
    std::array<std::size_t, kFixedColumns.size()> fixedColumns{};

    std::transform(
        kFixedColumns.begin(),
        kFixedColumns.end(),
        fixedColumns.begin(),
        [&table](const FixedColumn& aColumn) { return table.columnIndex(aColumn.mName); });

    aParsed.mInfo.mSourceFile = aSourceFile;
    aParsed.mInfo.mSha256 = sha256Hex(content);

    std::set<std::pair<std::string, Date>> seenPairs;

    for (const CsvRow& row : table.mRows)
    {
        const std::size_t rowIndex = row.mRowNumber;

        const auto reject =
            [&](DiagnosticCode aCode, DiagnosticMessage aMessage, std::string aField) {
                return rateDiagnostic(aSourceFile,
                                      aCode,
                                      std::move(aMessage),
                                      rowIndex,
                                      std::move(aField));
            };

        const auto wrongColumn =
            std::find_if(kFixedColumns.begin(),
                         kFixedColumns.end(),
                         [&row, &fixedColumns](const FixedColumn& aColumn) {
                             return row.mFields[fixedColumns[&aColumn - kFixedColumns.data()]] !=
                                    aColumn.mExpected;
                         });

        if (wrongColumn != kFixedColumns.end())
        {
            return reject(
                DiagnosticCode::InvalidValue,
                makeDiagnosticMessage(MessageKey::InvalidValueRateFieldExpected,
                                      {textParam("field", std::string{wrongColumn->mName}),
                                       textParam("expected", std::string{wrongColumn->mExpected})}),
                std::string{wrongColumn->mName});
        }

        const std::string currency{row.mFields[currencyColumn]};

        if (!isCurrencyCode(currency))
        {
            return reject(DiagnosticCode::InvalidValue,
                          makeDiagnosticMessage(MessageKey::InvalidValueRateCurrency),
                          "CURRENCY");
        }

        const std::string expectedKey = "EXR.D." + currency + ".EUR.SP00.A";

        if (row.mFields[keyColumn] != expectedKey)
        {
            return reject(DiagnosticCode::InvalidValue,
                          makeDiagnosticMessage(
                              MessageKey::InvalidValueRateFieldExpected,
                              {textParam("field", "KEY"), textParam("expected", expectedKey)}),
                          "KEY");
        }

        const auto observationDate = parseCalendarDate(row.mFields[dateColumn]);

        if (!observationDate)
        {
            return reject(DiagnosticCode::InvalidValue,
                          makeDiagnosticMessage(MessageKey::InvalidValueRateDate),
                          "TIME_PERIOD");
        }

        if (row.mFields[statusColumn] != kNormalStatus)
        {
            return reject(
                DiagnosticCode::InvalidValue,
                makeDiagnosticMessage(MessageKey::InvalidValueRateFieldExpected,
                                      {textParam("field", "OBS_STATUS"),
                                       textParam("expected", std::string{kNormalStatus})}),
                "OBS_STATUS");
        }

        const std::string_view valueText = row.mFields[valueColumn];

        if (valueText.empty())
        {
            return reject(DiagnosticCode::MissingField,
                          makeDiagnosticMessage(MessageKey::MissingFieldRateValue),
                          "OBS_VALUE");
        }

        const auto value = importExchangeRate(valueText);
        const auto* rateValue = std::get_if<ExchangeRate>(&value);

        if (rateValue == nullptr)
        {
            return reject(DiagnosticCode::InvalidValue,
                          makeDiagnosticMessage(MessageKey::InvalidValueRateValue),
                          "OBS_VALUE");
        }

        // The date was validated, so its first four characters are digits.
        const int year = std::stoi(std::string{row.mFields[dateColumn].substr(0, 4)});

        if (aParsed.mRates.empty())
        {
            aParsed.mInfo.mYear = year;
        }

        if (year != aParsed.mInfo.mYear)
        {
            return reject(DiagnosticCode::MixedRateYears,
                          makeDiagnosticMessage(MessageKey::MixedRateYears,
                                                {numberParam("firstYear", aParsed.mInfo.mYear),
                                                 numberParam("foundYear", year)}),
                          "TIME_PERIOD");
        }

        if (!seenPairs.emplace(currency, *observationDate).second)
        {
            return reject(DiagnosticCode::DuplicateRate,
                          makeDiagnosticMessage(MessageKey::DuplicateRate),
                          "TIME_PERIOD");
        }

        aParsed.mInfo.mFirstDate = aParsed.mRates.empty()
                                       ? *observationDate
                                       : std::min(aParsed.mInfo.mFirstDate, *observationDate);
        aParsed.mInfo.mLastDate = std::max(aParsed.mInfo.mLastDate, *observationDate);

        aParsed.mRates.push_back(OfficialRate{
            .mCurrencyCode = currency,
            .mObservationDate = *observationDate,
            .mValue = *rateValue,
            .mSeriesKey = expectedKey,
            .mFileSha256 = aParsed.mInfo.mSha256,
            .mSourceFile = aSourceFile,
            .mSourceRow = rowIndex,
        });
    }

    // A fault of a row stops the rows after it; the rows before it were checked above.
    if (table.mFault)
    {
        return faultDiagnostic(aSourceFile, *table.mFault);
    }

    if (aParsed.mRates.empty())
    {
        return rateDiagnostic(aSourceFile,
                              DiagnosticCode::NoRates,
                              makeDiagnosticMessage(MessageKey::NoRates));
    }

    aParsed.mInfo.mRateCount = aParsed.mRates.size();

    return std::nullopt;
}

} // namespace

const OfficialRate* OfficialRateTable::find(std::string_view aCurrencyCode, Date aDate) const {
    const auto found = mRates.find({std::string{aCurrencyCode}, aDate});

    return found == mRates.end() ? nullptr : &found->second;
}

const OfficialRate* OfficialRateTable::find(Currency aCurrency, Date aDate) const {
    // EUR has no rate, and an unknown currency has no code.
    if (aCurrency == Currency::EUR || aCurrency == Currency::Unknown)
    {
        return nullptr;
    }

    return find(currencyCode(aCurrency), aDate);
}

OfficialRateLoadResult loadEcbRateFiles(const std::vector<std::filesystem::path>& aPaths) {
    OfficialRateLoadResult result;
    std::vector<ParsedRateFile> accepted;

    for (const auto& path : aPaths)
    {
        const SourceFilename sourceFile = displayName(path);
        ParsedRateFile parsed;
        std::optional<ParseDiagnostic> fault;

        // The CSV reader throws on input it cannot read, such as a row longer than two 10 MB
        // chunks. That rejects only this file.
        try
        { fault = readRateFile(path, sourceFile, parsed); } catch (const std::runtime_error&)
        {
            fault = rateDiagnostic(sourceFile,
                                   DiagnosticCode::ParseError,
                                   makeDiagnosticMessage(MessageKey::ParseErrorUnreadableRateFile));
        }

        if (fault)
        {
            result.mDiagnostics.push_back(std::move(*fault));
            continue;
        }

        accepted.push_back(std::move(parsed));
    }

    std::map<int, std::size_t> filesPerYear;

    for (const auto& file : accepted)
    {
        ++filesPerYear[file.mInfo.mYear];
    }

    for (auto& file : accepted)
    {
        if (filesPerYear[file.mInfo.mYear] > 1)
        {
            result.mDiagnostics.push_back(
                rateDiagnostic(file.mInfo.mSourceFile,
                               DiagnosticCode::DuplicateRateYear,
                               makeDiagnosticMessage(MessageKey::DuplicateRateYear,
                                                     {numberParam("year", file.mInfo.mYear)})));
            continue;
        }

        for (auto& rate : file.mRates)
        {
            auto key = std::make_pair(rate.mCurrencyCode, rate.mObservationDate);

            result.mTable.mRates.emplace(std::move(key), std::move(rate));
        }

        result.mTable.mFiles.push_back(std::move(file.mInfo));
    }

    return result;
}

} // namespace taxbroker
