#pragma once

#include "taxbroker/event_metadata.hpp"
#include "taxbroker/exact_arithmetic.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace taxbroker {

/// The date from `YYYY-MM-DD`, or none when the text is not a real calendar date.
[[nodiscard]] std::optional<Date> parseCalendarDate(std::string_view aValue);

/// The date as YYYY-MM-DD, for a message parameter.
[[nodiscard]] std::string formatCalendarDate(Date aDate);

/// The exact text of a value stored at `aScale` (a power of ten). Trailing zeros are removed only
/// down to `aMinimumDecimals`, and nothing is rounded. At UNITS_SCALE, `150'000'000` is
/// `1.50000000` with 8 and `1.5000` with 4. Throws `std::invalid_argument` when `aScale` is not a
/// power of ten or `aMinimumDecimals` is more than the decimals it stores.
[[nodiscard]] std::string
formatFixedPoint(std::int64_t aValue, std::int64_t aScale, std::size_t aMinimumDecimals);

/// Stored value plus source facts that would otherwise be lost during import rounding.
struct ParsedFixedPoint {
    std::int64_t mValue{};           ///< The value stored at the requested scale.
    bool mSourceNegative{};          ///< The source text had a minus sign.
    bool mSourceNonzero{};           ///< The source decimal was not zero.
    std::size_t mFractionalDigits{}; ///< Digits after the decimal point in the source.
    std::string mDiscardedDigits;    ///< Source digits beyond the stored scale.
    std::string mCanonicalValue;     ///< Exact signed source decimal without redundant zeros.
    bool mRoundedAwayFromZero{};     ///< Import rounding increased the magnitude.
};

/// Accepts only `[+|-]digits[.digits]` with optional surrounding spaces: no digit grouping, no
/// decimal comma and no accounting parentheses.
/// Rounds once, halves away from zero; optionally rejects excess precision or nonzero-to-zero loss.
[[nodiscard]] NumericResult<ParsedFixedPoint>
importFixedPoint(std::string_view aValue,
                 std::int64_t aScale,
                 ValuePolicy aPolicy = ValuePolicy::Signed,
                 bool aRejectNonzeroRoundedToZero = true,
                 bool aAllowExtraPrecision = true);

} // namespace taxbroker
