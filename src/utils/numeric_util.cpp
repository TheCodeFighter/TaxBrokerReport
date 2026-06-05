#include "numeric_util.hpp"

taxbroker::Money parseMoney4(std::string_view value) {
    return static_cast<taxbroker::Money>(std::stoll(std::string(value)) * taxbroker::MONEY_SCALE);
}

taxbroker::Units parseUnits8(std::string_view value) {
    return static_cast<taxbroker::Units>(std::stoll(std::string(value)) * taxbroker::UNITS_SCALE);
}

taxbroker::CorpRatio parseCorpRatio8(std::string_view value) {
    return static_cast<taxbroker::CorpRatio>(std::stoll(std::string(value)) *
                                             taxbroker::CORP_RATIO_SCALE);
}

taxbroker::Money multiplyMoneyUnits(taxbroker::Money price, taxbroker::Units units) {
    // To avoid overflow, we perform the multiplication in 128-bit space if available.
    // If not available, we can use double as a fallback, but it may introduce precision issues for
    // very large values.
#if defined(__GNUC__) || defined(__clang__)
    __int128 result = static_cast<__int128>(price) * static_cast<__int128>(units);
    return static_cast<taxbroker::Money>(result / taxbroker::UNITS_SCALE);
#else
    double result =
        static_cast<double>(price) * static_cast<double>(units) / taxbroker::UNITS_SCALE;
    return static_cast<taxbroker::Money>(result);
#endif
}