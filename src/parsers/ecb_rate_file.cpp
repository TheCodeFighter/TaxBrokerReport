#include "taxbroker/official_rates.hpp"

#include "taxbroker/diagnostic_messages.hpp"
#include "utils/date_utils.hpp"
#include "utils/numeric_util.hpp"
#include "utils/sha256.hpp"

#include <csv.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
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

/// An odd number of quote characters means a quoted field never closes, and the reader would
/// merge the following records into it without any error.
bool hasUnclosedQuote(std::string_view aContent) {
    return std::count(aContent.begin(), aContent.end(), '"') % 2 != 0;
}

/// A line break inside a field means a quote opened on one line and closed on a later one. The
/// reader then merges the lines into one record, which can still have the right field count.
bool hasLineBreakInsideField(const csv::CSVRow& aRow) {
    return std::any_of(aRow.begin(), aRow.end(), [](const csv::CSVField& aField) {
        return aField.get_sv().find_first_of("\r\n") != std::string_view::npos;
    });
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

    if (hasUnclosedQuote(content))
    {
        return rateDiagnostic(aSourceFile,
                              DiagnosticCode::ParseError,
                              makeDiagnosticMessage(MessageKey::ParseErrorUnclosedQuote));
    }

    // KEEP delivers every record. The default silently drops records whose field count differs
    // from the header.
    csv::CSVFormat format;

    format.delimiter(',').variable_columns(csv::VariableColumnPolicy::KEEP);

    // Not csv::parse: its reader keeps a reference to a stream that ends when csv::parse returns,
    // and reads it again after the first 10 MB chunk. This stream lives as long as the reader.
    std::stringstream stream{content};
    csv::CSVReader reader{stream, format};
    const auto headerNames = reader.get_col_names();
    std::string missingColumns;
    std::size_t missingCount = 0;

    for (const std::string_view column : kRequiredColumns)
    {
        if (std::find(headerNames.begin(), headerNames.end(), column) == headerNames.end())
        {
            missingColumns += missingCount == 0 ? "" : ", ";
            missingColumns += column;
            ++missingCount;
        }
    }

    if (missingCount != 0)
    {
        return rateDiagnostic(aSourceFile,
                              DiagnosticCode::InvalidHeader,
                              makeDiagnosticMessage(MessageKey::InvalidHeaderRateFileColumns,
                                                    {numberParam("count", missingCount),
                                                     textParam("columns", missingColumns)}));
    }

    // Two columns with one name leave it unclear which one holds the value.
    const auto repeatedColumn =
        std::find_if(kRequiredColumns.begin(),
                     kRequiredColumns.end(),
                     [&headerNames](std::string_view aColumn) {
                         return std::count(headerNames.begin(), headerNames.end(), aColumn) > 1;
                     });

    if (repeatedColumn != kRequiredColumns.end())
    {
        return rateDiagnostic(
            aSourceFile,
            DiagnosticCode::InvalidHeader,
            makeDiagnosticMessage(MessageKey::InvalidHeaderRateFileDuplicateColumn,
                                  {textParam("column", std::string{*repeatedColumn})}));
    }

    aParsed.mInfo.mSourceFile = aSourceFile;
    aParsed.mInfo.mSha256 = sha256Hex(content);

    std::set<std::pair<std::string, Date>> seenPairs;
    std::size_t rowIndex = 1;

    for (const csv::CSVRow& row : reader)
    {
        ++rowIndex;

        const auto reject =
            [&](DiagnosticCode aCode, DiagnosticMessage aMessage, std::string aField) {
                return rateDiagnostic(aSourceFile,
                                      aCode,
                                      std::move(aMessage),
                                      rowIndex,
                                      std::move(aField));
            };

        if (hasLineBreakInsideField(row))
        {
            return rateDiagnostic(aSourceFile,
                                  DiagnosticCode::ParseError,
                                  makeDiagnosticMessage(MessageKey::ParseErrorUnclosedQuote),
                                  rowIndex);
        }

        if (row.size() != headerNames.size())
        {
            return rateDiagnostic(
                aSourceFile,
                DiagnosticCode::FieldCountMismatch,
                makeDiagnosticMessage(MessageKey::FieldCountMismatchRateFile,
                                      {numberParam("expected", headerNames.size()),
                                       numberParam("found", row.size())}),
                rowIndex);
        }

        const auto wrongColumn =
            std::find_if(kFixedColumns.begin(),
                         kFixedColumns.end(),
                         [&row](const FixedColumn& aColumn) {
                             return row[std::string{aColumn.mName}].get_sv() != aColumn.mExpected;
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

        const std::string currency{row["CURRENCY"].get_sv()};

        if (!isCurrencyCode(currency))
        {
            return reject(DiagnosticCode::InvalidValue,
                          makeDiagnosticMessage(MessageKey::InvalidValueRateCurrency),
                          "CURRENCY");
        }

        const std::string expectedKey = "EXR.D." + currency + ".EUR.SP00.A";

        if (row["KEY"].get_sv() != expectedKey)
        {
            return reject(DiagnosticCode::InvalidValue,
                          makeDiagnosticMessage(
                              MessageKey::InvalidValueRateFieldExpected,
                              {textParam("field", "KEY"), textParam("expected", expectedKey)}),
                          "KEY");
        }

        const auto observationDate = parseCalendarDate(row["TIME_PERIOD"].get_sv());

        if (!observationDate)
        {
            return reject(DiagnosticCode::InvalidValue,
                          makeDiagnosticMessage(MessageKey::InvalidValueRateDate),
                          "TIME_PERIOD");
        }

        if (row["OBS_STATUS"].get_sv() != kNormalStatus)
        {
            return reject(
                DiagnosticCode::InvalidValue,
                makeDiagnosticMessage(MessageKey::InvalidValueRateFieldExpected,
                                      {textParam("field", "OBS_STATUS"),
                                       textParam("expected", std::string{kNormalStatus})}),
                "OBS_STATUS");
        }

        const std::string_view valueText = row["OBS_VALUE"].get_sv();

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
        const int year = std::stoi(std::string{row["TIME_PERIOD"].get_sv().substr(0, 4)});

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
    switch (aCurrency)
    {
    case Currency::USD:
        return find("USD", aDate);
    case Currency::GBP:
        return find("GBP", aDate);
    case Currency::CHF:
        return find("CHF", aDate);
    case Currency::JPY:
        return find("JPY", aDate);
    case Currency::EUR:
    case Currency::Unknown:
        return nullptr;
    }

    return nullptr;
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
