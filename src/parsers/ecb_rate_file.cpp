#include "taxbroker/official_rates.hpp"

#include "taxbroker/currency.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "utils/csv_text_reader.hpp"
#include "utils/date_utils.hpp"
#include "utils/numeric_util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <string>
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
    // A folder opens on Linux and fails only while it is read, so it is checked first.
    std::error_code fileError;
    const bool regularFile = std::filesystem::is_regular_file(aPath, fileError);
    std::ifstream input{aPath, std::ios::binary};

    if (!regularFile || !input)
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
    aParsed.mInfo.mOrigin = OfficialRateOrigin::Supplied;

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
            .mSourceFile = aSourceFile,
            .mSourceRow = rowIndex,
            .mSource = OfficialRateSource::EcbReferenceRateFile,
            .mOrigin = OfficialRateOrigin::Supplied,
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

/// The columns of a compact built-in rate file.
constexpr std::array<std::string_view, 3> kBuiltInColumns{"currency", "date", "rate"};

/// Throws the build error of a built-in file: its name, the logical row and the reason.
[[noreturn]] void
failBuiltIn(std::string_view aFile, std::optional<std::size_t> aRow, std::string_view aReason) {
    std::string text{aFile};

    if (aRow)
    {
        text += " row " + std::to_string(*aRow);
    }

    throw std::logic_error{text + ": " + std::string{aReason}};
}

/// The built-in files, parsed once. A fault is a build error, so this throws on the first use.
const std::vector<std::shared_ptr<const BuiltInRateYear>>& builtInYears() {
    static const std::vector<std::shared_ptr<const BuiltInRateYear>> years = [] {
        std::vector<std::shared_ptr<const BuiltInRateYear>> parsed;

        for (const BuiltInRateFile& file : builtInEcbRateFiles())
        {
            auto year = std::make_shared<BuiltInRateYear>();

            year->mTable = parseBuiltInRates(file);

            for (const auto& entry : year->mTable.mRates)
            {
                year->mPublicationDays.insert(entry.first.second);
            }

            parsed.push_back(std::move(year));
        }

        return parsed;
    }();

    return years;
}

} // namespace

OfficialRateTable parseBuiltInRates(const BuiltInRateFile& aFile) {
    const CsvTable table = readCsvText(aFile.mText, ',', kBuiltInColumns);

    if (table.mFault && (table.mFault->mKind == CsvFaultKind::UnclosedQuote ||
                         table.mFault->mKind == CsvFaultKind::MissingColumns ||
                         table.mFault->mKind == CsvFaultKind::RepeatedColumn))
    {
        failBuiltIn(aFile.mName, std::nullopt, "the header is not valid");
    }

    const std::size_t currencyColumn = table.columnIndex("currency");
    const std::size_t dateColumn = table.columnIndex("date");
    const std::size_t rateColumn = table.columnIndex("rate");

    OfficialRateTable result;
    OfficialRateFile info;

    info.mSourceFile = SourceFilename::fromPath(aFile.mName);
    info.mYear = aFile.mYear;
    info.mOrigin = OfficialRateOrigin::BuiltIn;

    for (const CsvRow& row : table.mRows)
    {
        const std::string& currency = row.mFields[currencyColumn];

        if (!isCurrencyCode(currency))
        {
            failBuiltIn(aFile.mName, row.mRowNumber, "the currency is not a code");
        }

        const auto observationDate = parseCalendarDate(row.mFields[dateColumn]);

        if (!observationDate)
        {
            failBuiltIn(aFile.mName, row.mRowNumber, "the date is not a calendar date");
        }

        // The date was validated, so its first four characters are digits.
        if (std::stoi(row.mFields[dateColumn].substr(0, 4)) != aFile.mYear)
        {
            failBuiltIn(aFile.mName, row.mRowNumber, "the date is in another year");
        }

        const auto value = importExchangeRate(row.mFields[rateColumn]);
        const auto* rateValue = std::get_if<ExchangeRate>(&value);

        if (rateValue == nullptr)
        {
            failBuiltIn(aFile.mName, row.mRowNumber, "the rate is not valid");
        }

        const bool first = result.mRates.empty();

        const auto inserted =
            result.mRates.emplace(std::make_pair(currency, *observationDate),
                                  OfficialRate{
                                      .mCurrencyCode = currency,
                                      .mObservationDate = *observationDate,
                                      .mValue = *rateValue,
                                      .mSeriesKey = "EXR.D." + currency + ".EUR.SP00.A",
                                      .mSourceFile = info.mSourceFile,
                                      .mSourceRow = row.mRowNumber,
                                      .mSource = OfficialRateSource::EcbReferenceRateFile,
                                      .mOrigin = OfficialRateOrigin::BuiltIn,
                                  });

        if (!inserted.second)
        {
            failBuiltIn(aFile.mName, row.mRowNumber, "the currency and date are given twice");
        }

        info.mFirstDate = first ? *observationDate : std::min(info.mFirstDate, *observationDate);
        info.mLastDate = first ? *observationDate : std::max(info.mLastDate, *observationDate);
    }

    if (table.mFault)
    {
        failBuiltIn(aFile.mName, table.mFault->mRowNumber, "the row is not valid");
    }

    if (result.mRates.empty())
    {
        failBuiltIn(aFile.mName, std::nullopt, "the file has no rates");
    }

    info.mRateCount = result.mRates.size();
    result.mFiles.push_back(std::move(info));

    return result;
}

const OfficialRate* OfficialRateTable::find(std::string_view aCurrencyCode, Date aDate) const {
    const std::pair<std::string, Date> key{std::string{aCurrencyCode}, aDate};
    const auto found = mRates.find(key);

    if (found != mRates.end())
    {
        return &found->second;
    }

    // A built-in year holds only the days of its own file.
    for (const auto& year : mBuiltInYears)
    {
        const OfficialRateFile& file = year->mTable.mFiles.front();

        if (aDate >= file.mFirstDate && aDate <= file.mLastDate)
        {
            const auto builtIn = year->mTable.mRates.find(key);

            return builtIn == year->mTable.mRates.end() ? nullptr : &builtIn->second;
        }
    }

    return nullptr;
}

std::set<Date> OfficialRateTable::publicationDays() const {
    std::set<Date> days;

    for (const auto& entry : mRates)
    {
        days.insert(entry.first.second);
    }

    for (const auto& year : mBuiltInYears)
    {
        days.insert(year->mPublicationDays.begin(), year->mPublicationDays.end());
    }

    return days;
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
        std::optional<ParseDiagnostic> fault = readRateFile(path, sourceFile, parsed);

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

OfficialRateLoadResult loadOfficialRates(const std::vector<std::filesystem::path>& aSuppliedPaths) {
    OfficialRateLoadResult result = loadEcbRateFiles(aSuppliedPaths);

    std::set<int> suppliedYears;

    for (const OfficialRateFile& file : result.mTable.mFiles)
    {
        suppliedYears.insert(file.mYear);
    }

    for (const auto& builtIn : builtInYears())
    {
        const OfficialRateFile& file = builtIn->mTable.mFiles.front();

        if (suppliedYears.contains(file.mYear))
        {
            auto replaced = std::find_if(result.mTable.mFiles.begin(),
                                         result.mTable.mFiles.end(),
                                         [&file](const OfficialRateFile& aSupplied) {
                                             return aSupplied.mYear == file.mYear;
                                         });

            replaced->mReplacesBuiltIn = true;
            continue;
        }

        result.mTable.mBuiltInYears.push_back(builtIn);
        result.mTable.mFiles.push_back(file);
    }

    std::sort(result.mTable.mFiles.begin(),
              result.mTable.mFiles.end(),
              [](const OfficialRateFile& aLeft, const OfficialRateFile& aRight) {
                  return aLeft.mYear < aRight.mYear;
              });

    return result;
}

} // namespace taxbroker
