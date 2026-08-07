#include "utils/numeric_util.hpp"

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

template<typename ScaledType, int64_t Scale>
std::optional<ScaledType> parseScaledNumber(std::string_view value) {
    if (value.empty()) return std::nullopt;

    std::string cleaned;
    cleaned.reserve(value.size());
    bool negative = false;

    for (char ch : value) {
        if (ch == ',') continue;
        if (ch == '(') { negative = true; continue; }
        if (ch == ')') continue;
        if (std::isspace(static_cast<unsigned char>(ch))) continue;
        if (ch == '.' && cleaned.find('.') == std::string::npos) { 
            cleaned += ch; 
            continue;
        }
        cleaned += ch;
    }

    if (cleaned.empty()) return std::nullopt;

    if (!cleaned.empty() && cleaned[0] == '-') {
        negative = true;
        cleaned.erase(0, 1);
    }

    int64_t val = 0;
    auto [ptr, ec] = std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), val);
    if (ec != std::errc{} || ptr != cleaned.data() + cleaned.size()) {
        return std::nullopt;
    }

    if (negative) val = -val;

#if defined(__GNUC__) || defined(__clang__)
    if constexpr (Scale != 1) {
        __int128 result = static_cast<__int128>(val) * Scale;
        return static_cast<ScaledType>(result);
    }
#endif
    return static_cast<ScaledType>(val * Scale);
}