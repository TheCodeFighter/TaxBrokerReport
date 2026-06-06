#pragma once

#include <string_view>
#include "taxbroker/types.hpp"

taxbroker::Money parseMoney4(std::string_view value);

taxbroker::Units parseUnits8(std::string_view value);

taxbroker::CorpRatio parseCorpRatio8(std::string_view value);

// To avoid unit64_t overflow when multiplying price and units
taxbroker::Money multiplyMoneyUnits(taxbroker::Money price, taxbroker::Units units);

template<typename ScaledType, int64_t Scale>
std::optional<ScaledType> parseScaledNumber(std::string_view value);