#pragma once

#include "taxbroker/diagnostics.hpp"
#include "taxbroker/input_file.hpp"
#include "taxbroker/types.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace taxbroker {

/// Where an official rate comes from. A broker rate is never one of these.
enum class OfficialRateSource {
    EcbReferenceRateFile,
};

/// Whether a rate or a rate file ships with the app or was supplied by the user.
enum class OfficialRateOrigin {
    BuiltIn,
    Supplied,
};

/// One official rate: `1 EUR = mValue` units of the currency on the observation date.
struct OfficialRate {
    std::string mCurrencyCode; ///< ISO 4217 code from the file, such as "USD".
    Date mObservationDate{};
    ExchangeRate mValue{};      ///< Foreign units per 1 EUR at EXCHANGE_RATE_SCALE; never rounded.
    std::string mSeriesKey;     ///< The ECB series key, such as "EXR.D.USD.EUR.SP00.A".
    SourceFilename mSourceFile; ///< The file name only, never the path.
    std::size_t mSourceRow{};   ///< One-based logical record; the header is row 1.
    OfficialRateSource mSource{}; ///< Always set by the code that creates the rate.
    OfficialRateOrigin mOrigin{}; ///< Built in or supplied; always set by the creator.
};

/// One rate that converted at least one amount, for the summary and the result.
struct UsedOfficialRate {
    Currency mCurrency{Currency::Unknown};
    Date mTaxDate{};
    OfficialRate mRate;
};

/// A rate file that was loaded.
struct OfficialRateFile {
    SourceFilename mSourceFile;
    int mYear{};
    Date mFirstDate{}; ///< The earliest observation date of any currency in the file.
    Date mLastDate{};  ///< The latest observation date of any currency in the file.
    std::size_t mRateCount{};
    OfficialRateOrigin mOrigin{}; ///< Built in or supplied; always set by the creator.
    bool mReplacesBuiltIn{};      ///< A supplied file whose year is also built in; it replaces it.
};

class BuiltInRateYear;

/// The days on which a table has a rate for any currency. It asks the built-in years only about
/// the days that fall in their own year, so a year is parsed only when a run needs it.
class PublicationDays {
  public:
    [[nodiscard]] bool contains(Date aDate) const;

  private:
    friend struct OfficialRateTable;

    std::set<Date> mOwn; ///< The days of the rates the table owns.
    std::vector<std::shared_ptr<const BuiltInRateYear>> mBuiltInYears;
};

/// The loaded official rates, found only by an exact currency and date.
struct OfficialRateTable {
    /// The rates this table owns: the supplied files, or the one file just parsed. The rates of
    /// the built-in years in use are not copied here; they are in mBuiltInYears.
    std::map<std::pair<std::string, Date>, OfficialRate> mRates;
    std::vector<OfficialRateFile> mFiles;
    /// The built-in years in use, shared read-only by every table. Each is parsed once per run
    /// of the program, and only when a run looks for a rate in its year.
    std::vector<std::shared_ptr<const BuiltInRateYear>> mBuiltInYears;

    /// The rate for exactly this currency code and date, or null. It never returns a nearby date.
    [[nodiscard]] const OfficialRate* find(std::string_view aCurrencyCode, Date aDate) const;

    /// Like the code lookup. EUR and Unknown have no official rate and return null.
    [[nodiscard]] const OfficialRate* find(Currency aCurrency, Date aDate) const;

    /// The days on which the table has a rate for any currency, own rates and built-in years.
    [[nodiscard]] PublicationDays publicationDays() const;
};

/// The rates of one calendar year.
struct BuiltInRateFile {
    int mYear{};
    std::string_view mName;
    std::string_view mText; ///< The text: the header `currency,date,rate` and one row per rate.
};

/**
 * One built-in year. Its file name, year, first and last date and rate count come from a cheap
 * scan when it is made; the rates are parsed once, on the first lookup in the year. The file is
 * part of the program, so a fault is a build error: the scan throws std::logic_error for a wrong
 * header, a row without three fields or a file without rates, and the parse throws it for any
 * other fault (see `parseBuiltInRates`).
 */
class BuiltInRateYear {
  public:
    explicit BuiltInRateYear(const BuiltInRateFile& aFile);

    BuiltInRateYear(const BuiltInRateYear&) = delete;
    BuiltInRateYear& operator=(const BuiltInRateYear&) = delete;

    /// The file as the scan found it; it matches the parsed file.
    [[nodiscard]] const OfficialRateFile& info() const {
        return mInfo;
    }

    /// Whether the rates were parsed already.
    [[nodiscard]] bool isParsed() const {
        return mIsParsed.load();
    }

    /// The rate for this key, or null. Parses the year on the first call.
    [[nodiscard]] const OfficialRate* find(const std::pair<std::string, Date>& aKey) const;

    /// Whether the year has a rate for any currency on this day. Parses the year on the first call.
    [[nodiscard]] bool hasPublicationDay(Date aDate) const;

    /// The parsed table of the year (one file, no built-in years of its own).
    [[nodiscard]] const OfficialRateTable& table() const;

  private:
    void parseOnce() const;

    BuiltInRateFile mFile;
    OfficialRateFile mInfo;
    mutable std::mutex mParseMutex; ///< Not std::call_once: on musl an exception aborts it.
    mutable std::atomic<bool> mIsParsed{false};
    mutable OfficialRateTable mTable;
    mutable std::set<Date> mDays;
};

struct OfficialRateLoadResult {
    OfficialRateTable mTable;
    /// Errors only. A file with an error adds no rate; other files still load.
    std::vector<ParseDiagnostic> mDiagnostics;
};

/// The embedded rate files, one per year, in year order.
[[nodiscard]] std::span<const BuiltInRateFile> builtInEcbRateFiles();

/**
 * Reads the text of one compact rate file into a table with that one file. Throws
 * std::logic_error with the file name and the logical row (header = 1) at the first fault: the
 * file is part of the program, so a fault is a build error.
 */
[[nodiscard]] OfficialRateTable parseBuiltInRates(const BuiltInRateFile& aFile);

/**
 * Loads ECB Data Portal `csvdata` exports of the `EXR` dataset, one file per calendar year.
 * A file is accepted whole or rejected whole at its first fault. Two files for the same year
 * that both pass the checks are both rejected.
 */
[[nodiscard]] OfficialRateLoadResult
loadEcbRateFiles(const std::vector<std::filesystem::path>& aPaths,
                 std::size_t aMaxFileBytes = kMaxInputFileBytes);

[[nodiscard]] OfficialRateLoadResult
loadEcbRateFiles(std::span<const InputFile> aFiles, std::size_t aMaxFileBytes = kMaxInputFileBytes);

/**
 * The rates of a run: the supplied files (loaded as `loadEcbRateFiles` does) and, for each year
 * no accepted supplied file covers, the built-in file of that year. A supplied file replaces the
 * whole built-in year, even when it has fewer days. The files are in year order. Only supplied
 * files can give diagnostics.
 */
[[nodiscard]] OfficialRateLoadResult
loadOfficialRates(const std::vector<std::filesystem::path>& aSuppliedPaths);

[[nodiscard]] OfficialRateLoadResult loadOfficialRates(std::span<const InputFile> aSuppliedFiles);

} // namespace taxbroker
