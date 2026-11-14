#pragma once

#include "taxbroker/types.hpp"

#include <cstddef>
#include <string>

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

} // namespace taxbroker
