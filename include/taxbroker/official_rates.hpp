#pragma once

#include "taxbroker/diagnostics.hpp"
#include "taxbroker/types.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace taxbroker {

/// Where an official rate comes from. A broker rate is never one of these.
enum class OfficialRateSource {
    EcbReferenceRateFile,
};

/// One official rate: `1 EUR = mValue` units of the currency on the observation date.
struct OfficialRate {
    std::string mCurrencyCode; ///< ISO 4217 code from the file, such as "USD".
    Date mObservationDate{};
    ExchangeRate mValue{};      ///< Foreign units per 1 EUR at EXCHANGE_RATE_SCALE; never rounded.
    std::string mSeriesKey;     ///< The ECB series key, such as "EXR.D.USD.EUR.SP00.A".
    std::string mFileSha256;    ///< SHA-256 of the file bytes, 64 lowercase hex digits.
    SourceFilename mSourceFile; ///< The file name only, never the path.
    std::size_t mSourceRow{};   ///< One-based logical record; the header is row 1.
    OfficialRateSource mSource{OfficialRateSource::EcbReferenceRateFile};
};

/// A rate file that was loaded.
struct OfficialRateFile {
    SourceFilename mSourceFile;
    std::string mSha256;
    int mYear{};
    std::size_t mRateCount{};
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

/**
 * Loads ECB Data Portal `csvdata` exports of the `EXR` dataset, one file per calendar year.
 * A file is accepted whole or rejected whole at its first fault. Two files for the same year
 * that both pass the checks are both rejected.
 */
[[nodiscard]] OfficialRateLoadResult
loadEcbRateFiles(const std::vector<std::filesystem::path>& aPaths);

} // namespace taxbroker
