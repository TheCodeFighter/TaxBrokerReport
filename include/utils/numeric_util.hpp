#include "types_def.hpp"


taxbroker::Money parseMoney4(std::string_view value);

taxbroker::Units parseUnits8(std::string_view value);

// To avoid unit64_t overflow when multiplying price and units
taxbroker::Money multiplyMoneyUnits(taxbroker::Money price, taxbroker::Units units);