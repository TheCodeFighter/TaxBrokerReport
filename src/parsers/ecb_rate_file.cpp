#include "parsers/ecb_rate_file.hpp"

#include "taxbroker/currency.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/input_file.hpp"
#include "utils/csv_text_reader.hpp"
#include "utils/date_utils.hpp"
#include "utils/numeric_util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
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

/// Checks the text of one file into aParsed and returns no diagnostic, or returns the first
/// fault. The checks are the rate-file rules in `calculations.md`.
std::optional<ParseDiagnostic>
checkRateText(std::string_view aText, const SourceFilename& aSourceFile, ParsedRateFile& aParsed) {
    const CsvTable table = readCsvText(aText, ',', kRequiredColumns);

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

/// The header line of a compact built-in rate file. The files are written by
/// `dump_ecb_rates.sh --compact`, so the header is exactly this.
constexpr std::string_view kBuiltInHeader = "currency,date,rate";

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

/// The text after the header line of a built-in file; throws when the header is not exact.
std::string_view builtInBody(const BuiltInRateFile& aFile) {
    const std::size_t headerEnd = aFile.mText.find('\n');

    if (aFile.mText.substr(0, headerEnd) != kBuiltInHeader)
    {
        failBuiltIn(aFile.mName, std::nullopt, "the header is not valid");
    }

    return headerEnd == std::string_view::npos ? std::string_view{}
                                               : aFile.mText.substr(headerEnd + 1);
}

/// The three fields of a row `currency,date,rate`. A quote, a missing or an extra comma makes the
/// row not valid: the files are written without quotes.
struct BuiltInRow {
    std::string_view mCurrency;
    std::string_view mDate;
    std::string_view mRate;
};

/// Reads the line at aPosition in one pass and moves aPosition past its line break. Throws the
/// build error of the row when it is not valid.
BuiltInRow readBuiltInRow(const BuiltInRateFile& aFile,
                          std::size_t aRow,
                          const char*& aPosition,
                          const char* aEnd) {
    const char* const lineStart = aPosition;
    const char* firstComma = nullptr;
    const char* secondComma = nullptr;
    bool isValid = true;
    const char* cursor = lineStart;

    for (; cursor != aEnd; ++cursor)
    {
        // Most characters (letters, digits, '.', '-') are above the comma and need no check.
        if (static_cast<unsigned char>(*cursor) > static_cast<unsigned char>(','))
        {
            continue;
        }

        if (*cursor == '\n')
        {
            break;
        }

        if (*cursor == ',')
        {
            if (firstComma == nullptr)
            {
                firstComma = cursor;
            }
            else if (secondComma == nullptr)
            {
                secondComma = cursor;
            }
            else
            {
                isValid = false;
            }
        }
        else if (*cursor == '"')
        {
            isValid = false;
        }
    }

    if (!isValid || secondComma == nullptr)
    {
        failBuiltIn(aFile.mName, aRow, "the row is not valid");
    }

    // Each field starts one character after its separator (a comma, or the line break for the
    // next row).
    constexpr std::ptrdiff_t kSeparatorSize = 1;

    aPosition = cursor == aEnd ? aEnd : cursor + kSeparatorSize;

    return BuiltInRow{
        .mCurrency = std::string_view{lineStart, firstComma},
        .mDate = std::string_view{firstComma + kSeparatorSize, secondComma},
        .mRate = std::string_view{secondComma + kSeparatorSize, cursor},
    };
}

/// True when the 10-character date at aLeft is before the one at aRight, as text.
bool dateTextBefore(const char* aLeft, const char* aRight) {
    for (std::size_t index = 0; index < 10; ++index)
    {
        if (aLeft[index] != aRight[index])
        {
            return static_cast<unsigned char>(aLeft[index]) <
                   static_cast<unsigned char>(aRight[index]);
        }
    }

    return false;
}

/// A built-in file as a cheap scan sees it: the year, the first and last date and the row count.
/// One pass over the text; the rows are checked only as far as the scan needs, and
/// `parseBuiltInRates` checks every value.
OfficialRateFile scanBuiltInFile(const BuiltInRateFile& aFile) {
    const std::string_view body = builtInBody(aFile);
    const char* position = body.data();
    const char* const end = body.data() + body.size();
    const char* first = nullptr;
    const char* last = nullptr;
    std::size_t count = 0;

    while (position != end)
    {
        const std::size_t row = count + 2; // the header is row 1
        const BuiltInRow fields = readBuiltInRow(aFile, row, position, end);

        if (fields.mDate.size() != 10)
        {
            failBuiltIn(aFile.mName, row, "the date is not a calendar date");
        }

        // The dates are written as YYYY-MM-DD, so the text compares like the dates.
        if (count == 0 || dateTextBefore(fields.mDate.data(), first))
        {
            first = fields.mDate.data();
        }

        if (count == 0 || dateTextBefore(last, fields.mDate.data()))
        {
            last = fields.mDate.data();
        }

        ++count;
    }

    if (count == 0)
    {
        failBuiltIn(aFile.mName, std::nullopt, "the file has no rates");
    }

    const auto firstDate = parseCalendarDate(std::string_view{first, 10});
    const auto lastDate = parseCalendarDate(std::string_view{last, 10});

    if (!firstDate || !lastDate)
    {
        failBuiltIn(aFile.mName, std::nullopt, "the date is not a calendar date");
    }

    OfficialRateFile info;

    info.mSourceFile = SourceFilename::fromPath(aFile.mName);
    info.mYear = aFile.mYear;
    info.mFirstDate = *firstDate;
    info.mLastDate = *lastDate;
    info.mRateCount = count;
    info.mOrigin = OfficialRateOrigin::BuiltIn;

    return info;
}

/// The rate of a built-in row as 8-decimal fixed point. A plain positive decimal (up to 10 integer
/// digits and 8 decimals, such as `1.2625`) is read directly; anything else goes through
/// `importExchangeRate`, which gives the same value and the same errors.
std::optional<ExchangeRate> builtInRateValue(std::string_view aText) {
    constexpr std::size_t kMaxIntegerDigits = 10;
    constexpr std::size_t kDecimals = 8; // EXCHANGE_RATE_SCALE is 10^8

    std::int64_t integerPart = 0;
    std::int64_t fractionPart = 0;
    std::size_t integerDigits = 0;
    std::size_t fractionDigits = 0;
    bool isAfterPoint = false;
    bool isPlain = !aText.empty();

    for (const char character : aText)
    {
        if (character == '.' && !isAfterPoint)
        {
            isAfterPoint = true;
        }
        else if (character >= '0' && character <= '9')
        {
            const std::int64_t digit = character - '0';

            if (isAfterPoint)
            {
                fractionPart = fractionPart * 10 + digit;
                ++fractionDigits;
            }
            else
            {
                integerPart = integerPart * 10 + digit;
                ++integerDigits;
            }

            // The loop stops growing the numbers once the text is too long for the plain form.
            if (integerDigits > kMaxIntegerDigits || fractionDigits > kDecimals)
            {
                isPlain = false;
                break;
            }
        }
        else
        {
            isPlain = false;
            break;
        }
    }

    if (isPlain && integerDigits > 0 && (isAfterPoint ? fractionDigits > 0 : true))
    {
        for (std::size_t digit = fractionDigits; digit < kDecimals; ++digit)
        {
            fractionPart *= 10;
        }

        const ExchangeRate value = integerPart * EXCHANGE_RATE_SCALE + fractionPart;

        if (value > 0)
        {
            return value;
        }
    }

    const NumericResult<ExchangeRate> imported = importExchangeRate(aText);
    const ExchangeRate* rateValue = std::get_if<ExchangeRate>(&imported);

    return rateValue == nullptr ? std::nullopt : std::optional<ExchangeRate>{*rateValue};
}

/// The built-in years, made once. Making one only scans its file; a fault there is a build error,
/// so this throws on the first use.
const std::vector<std::shared_ptr<const BuiltInRateYear>>& builtInYears() {
    static const std::vector<std::shared_ptr<const BuiltInRateYear>> years = [] {
        const auto files = builtInEcbRateFiles();
        std::vector<std::shared_ptr<const BuiltInRateYear>> made;

        std::transform(files.begin(),
                       files.end(),
                       std::back_inserter(made),
                       [](const BuiltInRateFile& aFile) {
                           return std::make_shared<const BuiltInRateYear>(aFile);
                       });

        return made;
    }();

    return years;
}

} // namespace

OfficialRateTable parseBuiltInRates(const BuiltInRateFile& aFile) {
    const std::string_view text = builtInBody(aFile);
    const char* position = text.data();
    const char* const end = text.data() + text.size();

    OfficialRateTable result;
    OfficialRateFile info;
    std::size_t row = 1; // the header is row 1
    // The rows are sorted by currency, so the series key is built once for each currency.
    std::string seriesCurrency;
    std::string seriesKey;

    info.mSourceFile = SourceFilename::fromPath(aFile.mName);
    info.mYear = aFile.mYear;
    info.mOrigin = OfficialRateOrigin::BuiltIn;

    while (position != end)
    {
        ++row;

        const BuiltInRow fields = readBuiltInRow(aFile, row, position, end);

        if (!isCurrencyCode(fields.mCurrency))
        {
            failBuiltIn(aFile.mName, row, "the currency is not a code");
        }

        const std::string currency{fields.mCurrency};
        const auto observationDate = parseCalendarDate(fields.mDate);

        if (!observationDate)
        {
            failBuiltIn(aFile.mName, row, "the date is not a calendar date");
        }

        // The date was validated, so its first four characters are digits.
        const int dateYear = (fields.mDate[0] - '0') * 1000 + (fields.mDate[1] - '0') * 100 +
                             (fields.mDate[2] - '0') * 10 + (fields.mDate[3] - '0');

        if (dateYear != aFile.mYear)
        {
            failBuiltIn(aFile.mName, row, "the date is in another year");
        }

        const std::optional<ExchangeRate> rateValue = builtInRateValue(fields.mRate);

        if (!rateValue)
        {
            failBuiltIn(aFile.mName, row, "the rate is not valid");
        }

        if (currency != seriesCurrency)
        {
            seriesCurrency = currency;
            seriesKey = "EXR.D." + currency + ".EUR.SP00.A";
        }

        const bool first = result.mRates.empty();
        const std::size_t sizeBefore = result.mRates.size();

        // The rows are sorted, so a new rate nearly always goes to the end.
        result.mRates.emplace_hint(result.mRates.end(),
                                   std::make_pair(currency, *observationDate),
                                   OfficialRate{
                                       .mCurrencyCode = currency,
                                       .mObservationDate = *observationDate,
                                       .mValue = *rateValue,
                                       .mSeriesKey = seriesKey,
                                       .mSourceFile = info.mSourceFile,
                                       .mSourceRow = row,
                                       .mSource = OfficialRateSource::EcbReferenceRateFile,
                                       .mOrigin = OfficialRateOrigin::BuiltIn,
                                   });

        if (result.mRates.size() == sizeBefore)
        {
            failBuiltIn(aFile.mName, row, "the currency and date are given twice");
        }

        info.mFirstDate = first ? *observationDate : std::min(info.mFirstDate, *observationDate);
        info.mLastDate = first ? *observationDate : std::max(info.mLastDate, *observationDate);
    }

    if (result.mRates.empty())
    {
        failBuiltIn(aFile.mName, std::nullopt, "the file has no rates");
    }

    info.mRateCount = result.mRates.size();
    result.mFiles.push_back(std::move(info));

    return result;
}

BuiltInRateYear::BuiltInRateYear(const BuiltInRateFile& aFile)
    : mFile{aFile}, mInfo{scanBuiltInFile(aFile)} {}

void BuiltInRateYear::parseOnce() const {
    if (mIsParsed.load())
    {
        return;
    }

    const std::lock_guard lock{mParseMutex};

    if (mIsParsed.load())
    {
        return;
    }

    // A fault throws and leaves the year unparsed, so the next lookup throws it again.
    OfficialRateTable parsed = parseBuiltInRates(mFile);

    for (const auto& entry : parsed.mRates)
    {
        mDays.insert(entry.first.second);
    }

    mTable = std::move(parsed);
    mIsParsed.store(true);
}

const OfficialRate* BuiltInRateYear::find(const std::pair<std::string, Date>& aKey) const {
    parseOnce();

    const auto found = mTable.mRates.find(aKey);

    return found == mTable.mRates.end() ? nullptr : &found->second;
}

bool BuiltInRateYear::hasPublicationDay(Date aDate) const {
    parseOnce();

    return mDays.contains(aDate);
}

const OfficialRateTable& BuiltInRateYear::table() const {
    parseOnce();

    return mTable;
}

namespace {

/// The built-in year whose file covers the date, or null.
const BuiltInRateYear*
builtInYearOf(const std::vector<std::shared_ptr<const BuiltInRateYear>>& aYears, Date aDate) {
    const auto found = std::find_if(aYears.begin(),
                                    aYears.end(),
                                    [aDate](const std::shared_ptr<const BuiltInRateYear>& aYear) {
                                        return aDate >= aYear->info().mFirstDate &&
                                               aDate <= aYear->info().mLastDate;
                                    });

    return found == aYears.end() ? nullptr : found->get();
}

} // namespace

const OfficialRate* OfficialRateTable::find(std::string_view aCurrencyCode, Date aDate) const {
    const std::pair<std::string, Date> key{std::string{aCurrencyCode}, aDate};
    const auto found = mRates.find(key);

    if (found != mRates.end())
    {
        return &found->second;
    }

    // A built-in year holds only the days of its own file.
    const BuiltInRateYear* year = builtInYearOf(mBuiltInYears, aDate);

    return year == nullptr ? nullptr : year->find(key);
}

PublicationDays OfficialRateTable::publicationDays() const {
    PublicationDays days;

    for (const auto& entry : mRates)
    {
        days.mOwn.insert(entry.first.second);
    }

    days.mBuiltInYears = mBuiltInYears;

    return days;
}

bool PublicationDays::contains(Date aDate) const {
    if (mOwn.contains(aDate))
    {
        return true;
    }

    const BuiltInRateYear* year = builtInYearOf(mBuiltInYears, aDate);

    return year != nullptr && year->hasPublicationDay(aDate);
}

const OfficialRate* OfficialRateTable::find(Currency aCurrency, Date aDate) const {
    // EUR has no rate, and an unknown currency has no code.
    if (aCurrency == Currency::EUR || aCurrency == Currency::Unknown)
    {
        return nullptr;
    }

    return find(currencyCode(aCurrency), aDate);
}

namespace {

/// Adds the diagnostic of a file that gave no text.
void addRateFault(OfficialRateLoadResult& aResult,
                  const SourceFilename& aSourceFile,
                  const InputFileFault aFault,
                  const std::size_t aIndex,
                  const std::size_t aMaxFileBytes) {
    if (aFault == InputFileFault::Unreadable)
    {
        aResult.mDiagnostics.push_back(
            rateDiagnostic(aSourceFile,
                           DiagnosticCode::ParseError,
                           makeDiagnosticMessage(MessageKey::ParseErrorUnreadableRateFile)));

        return;
    }

    aResult.mDiagnostics.push_back(
        inputFileFaultDiagnostic(aFault, aSourceFile.value(), "ecb_rates", aIndex, aMaxFileBytes));
}

/// Adds the error of a file without a valid file name (no name in the diagnostic).
void addInvalidRateName(OfficialRateLoadResult& aResult) {
    aResult.mDiagnostics.push_back(
        rateDiagnostic(SourceFilename{},
                       DiagnosticCode::ParseError,
                       makeDiagnosticMessage(MessageKey::ParseErrorInvalidFilename)));
}

/// Checks the text of one file; an accepted file is added to aAccepted, else a diagnostic.
void addRateText(OfficialRateLoadResult& aResult,
                 std::vector<ParsedRateFile>& aAccepted,
                 const SourceFilename& aSourceFile,
                 const std::string_view aText,
                 const std::size_t aIndex,
                 const std::size_t aMaxFileBytes) {
    if (aText.size() > aMaxFileBytes)
    {
        addRateFault(aResult, aSourceFile, InputFileFault::TooLarge, aIndex, aMaxFileBytes);

        return;
    }

    try
    {
        ParsedRateFile parsed;
        std::optional<ParseDiagnostic> fault = checkRateText(aText, aSourceFile, parsed);

        if (fault)
        {
            aResult.mDiagnostics.push_back(std::move(*fault));

            return;
        }

        aAccepted.push_back(std::move(parsed));
    } catch (const std::bad_alloc&)
    { addRateFault(aResult, aSourceFile, InputFileFault::OutOfMemory, aIndex, aMaxFileBytes); }
}

/// Rejects two files of one year and puts the rates of the accepted files into the table.
OfficialRateLoadResult finishRateLoad(OfficialRateLoadResult aResult,
                                      std::vector<ParsedRateFile> aAccepted) {
    std::map<int, std::size_t> filesPerYear;

    for (const auto& file : aAccepted)
    {
        ++filesPerYear[file.mInfo.mYear];
    }

    for (auto& file : aAccepted)
    {
        if (filesPerYear[file.mInfo.mYear] > 1)
        {
            aResult.mDiagnostics.push_back(
                rateDiagnostic(file.mInfo.mSourceFile,
                               DiagnosticCode::DuplicateRateYear,
                               makeDiagnosticMessage(MessageKey::DuplicateRateYear,
                                                     {numberParam("year", file.mInfo.mYear)})));
            continue;
        }

        for (auto& rate : file.mRates)
        {
            auto key = std::make_pair(rate.mCurrencyCode, rate.mObservationDate);

            aResult.mTable.mRates.emplace(std::move(key), std::move(rate));
        }

        aResult.mTable.mFiles.push_back(std::move(file.mInfo));
    }

    return aResult;
}

} // namespace

OfficialRateLoadResult loadEcbRateFiles(const std::span<const InputFile> aFiles,
                                        const std::size_t aMaxFileBytes) {
    OfficialRateLoadResult result;
    std::vector<ParsedRateFile> accepted;

    for (std::size_t index = 0; index < aFiles.size(); ++index)
    {
        const auto sourceFile = SourceFilename::tryFromPath(aFiles[index].mName);

        if (!sourceFile)
        {
            addInvalidRateName(result);

            continue;
        }

        addRateText(result, accepted, *sourceFile, aFiles[index].mContent, index, aMaxFileBytes);
    }

    return finishRateLoad(std::move(result), std::move(accepted));
}

OfficialRateLoadResult loadEcbRateFiles(const std::vector<std::filesystem::path>& aPaths,
                                        const std::size_t aMaxFileBytes) {
    OfficialRateLoadResult result;
    std::vector<ParsedRateFile> accepted;

    // One file at a time, so only one file is in memory at once.
    for (std::size_t index = 0; index < aPaths.size(); ++index)
    {
        const auto validName = SourceFilename::tryFromPath(aPaths[index].filename().string());

        // A path with no file name is reported without reading anything.
        if (!validName)
        {
            addInvalidRateName(result);

            continue;
        }

        const SourceFilename& sourceFile = *validName;
        const auto read = readInputFile(aPaths[index], aMaxFileBytes);

        if (const auto* fault = std::get_if<InputFileFault>(&read))
        {
            addRateFault(result, sourceFile, *fault, index, aMaxFileBytes);

            continue;
        }

        addRateText(result,
                    accepted,
                    sourceFile,
                    std::get<InputFile>(read).mContent,
                    index,
                    aMaxFileBytes);
    }

    return finishRateLoad(std::move(result), std::move(accepted));
}

namespace {

/// Adds the built-in years that no accepted supplied file covers, in year order.
OfficialRateLoadResult addBuiltInYears(OfficialRateLoadResult aSupplied) {
    OfficialRateLoadResult result = std::move(aSupplied);

    std::set<int> suppliedYears;

    for (const OfficialRateFile& file : result.mTable.mFiles)
    {
        suppliedYears.insert(file.mYear);
    }

    for (const auto& builtIn : builtInYears())
    {
        const OfficialRateFile& file = builtIn->info();

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

} // namespace

OfficialRateLoadResult loadOfficialRates(const std::span<const InputFile> aSuppliedFiles) {
    return addBuiltInYears(loadEcbRateFiles(aSuppliedFiles));
}

OfficialRateLoadResult loadOfficialRates(const std::vector<std::filesystem::path>& aSuppliedPaths) {
    return addBuiltInYears(loadEcbRateFiles(aSuppliedPaths));
}

} // namespace taxbroker
