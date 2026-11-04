#pragma once

#include "taxbroker/diagnostics.hpp"
#include "taxbroker/types.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
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

/// The loaded official rates, found only by an exact currency and date.
struct OfficialRateTable {
    std::map<std::pair<std::string, Date>, OfficialRate> mRates;
    std::vector<OfficialRateFile> mFiles;

    /// The rate for exactly this currency code and date, or null. It never returns a nearby date.
    [[nodiscard]] const OfficialRate* find(std::string_view aCurrencyCode, Date aDate) const;

    /// Like the code lookup. EUR and Unknown have no official rate and return null.
    [[nodiscard]] const OfficialRate* find(Currency aCurrency, Date aDate) const;
};

struct OfficialRateLoadResult {
    OfficialRateTable mTable;
    /// Errors only. A file with an error adds no rate; other files still load.
    std::vector<ParseDiagnostic> mDiagnostics;
};

/// The rates of one calendar year.
struct BuiltInRateFile {
    int mYear{};
    std::string_view mName;
    std::string_view mText; ///< The text: a header `currency,date,rate` and one row per rate.
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
loadEcbRateFiles(const std::vector<std::filesystem::path>& aPaths);

/**
 * The rates of a run: the supplied files (loaded as `loadEcbRateFiles` does) and, for each year
 * no accepted supplied file covers, the built-in file of that year. A supplied file replaces the
 * whole built-in year, even when it has fewer days. The files are in year order. Only supplied
 * files can give diagnostics.
 */
[[nodiscard]] OfficialRateLoadResult
loadOfficialRates(const std::vector<std::filesystem::path>& aSuppliedPaths);

} // namespace taxbroker
