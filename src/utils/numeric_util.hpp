#pragma once

#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/text_format.hpp"
#include "taxbroker/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

taxbroker::Money parseMoney4(std::string_view aValue);

taxbroker::Units parseUnits8(std::string_view aValue);

taxbroker::CorpRatio parseCorpRatio8(std::string_view aValue);

bool parseInteger(std::string_view aValue, int& aResult);

// Returns nullopt when the scaled result cannot fit in Money.
std::optional<taxbroker::Money> multiplyUnitPriceUnits(taxbroker::UnitPrice aPrice,
                                                       taxbroker::Units aUnits);

namespace numeric_detail {

std::optional<std::int64_t> parseScaledInt64(std::string_view aValue, std::int64_t aScale);

} // namespace numeric_detail

template <typename ScaledType, std::int64_t Scale>
std::optional<ScaledType> parseScaledNumber(std::string_view aValue) {
    static_assert(std::is_same_v<ScaledType, std::int64_t>,
                  "Fixed-point storage types must currently use std::int64_t");
    static_assert(Scale > 0, "Fixed-point scale must be positive");

    return numeric_detail::parseScaledInt64(aValue, Scale);
}

namespace taxbroker {

/// Requires a positive rate; extra precision needs explicit rounding permission.
[[nodiscard]] NumericResult<ExchangeRate> importExchangeRate(std::string_view aValue,
                                                             bool aAllowExtraPrecision = false);

/// Links pre-normalization quantity evidence to its input row.
[[nodiscard]] UnitSourceEvidence unitSourceEvidence(const ParsedFixedPoint& aParsed,
                                                    std::string_view aSourceText,
                                                    SourceReference aSource);

} // namespace taxbroker
