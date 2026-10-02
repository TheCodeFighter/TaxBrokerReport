#include "utils/numeric_util.hpp"
#include "taxbroker/exact_arithmetic.hpp"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace {

constexpr std::uint64_t signedMagnitudeLimit(bool aNegative) {
    constexpr auto positiveLimit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return aNegative ? positiveLimit + 1U : positiveLimit;
}

std::optional<std::int64_t> applySign(std::uint64_t aMagnitude, bool aNegative) {
    const auto limit = signedMagnitudeLimit(aNegative);
    if (aMagnitude > limit)
        return std::nullopt;

    if (!aNegative)
        return static_cast<std::int64_t>(aMagnitude);
    if (aMagnitude == limit)
        return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t>(aMagnitude);
}

std::string_view trim(std::string_view aValue) {
    while (!aValue.empty() && std::isspace(static_cast<unsigned char>(aValue.front())))
    {
        aValue.remove_prefix(1);
    }
    while (!aValue.empty() && std::isspace(static_cast<unsigned char>(aValue.back())))
    {
        aValue.remove_suffix(1);
    }
    return aValue;
}

bool hasValidIntegerGrouping(std::string_view aValue) {
    const auto firstComma = aValue.find(',');
    if (firstComma == std::string_view::npos)
        return true;
    if (firstComma == 0 || firstComma > 3)
        return false;

    std::size_t groupLength = 0;
    for (std::size_t index = firstComma + 1; index < aValue.size(); ++index)
    {
        if (aValue[index] == ',')
        {
            if (groupLength != 3)
                return false;
            groupLength = 0;
        }
        else
        {
            ++groupLength;
        }
    }

    return groupLength == 3;
}

std::int64_t parseOrThrow(std::string_view aValue, std::int64_t aScale) {
    const auto parsed = numeric_detail::parseScaledInt64(aValue, aScale);
    if (!parsed)
        throw std::invalid_argument("Invalid or out-of-range fixed-point number");
    return *parsed;
}

} // namespace

taxbroker::Money parseMoney4(std::string_view aValue) {
    return parseOrThrow(aValue, taxbroker::MONEY_SCALE);
}

taxbroker::Units parseUnits8(std::string_view aValue) {
    return parseOrThrow(aValue, taxbroker::UNITS_SCALE);
}

taxbroker::CorpRatio parseCorpRatio8(std::string_view aValue) {
    return parseOrThrow(aValue, taxbroker::CORP_RATIO_SCALE);
}

bool parseInteger(std::string_view aValue, int& aResult) {
    const auto* end = aValue.data() + aValue.size();
    const auto [parsedEnd, error] = std::from_chars(aValue.data(), end, aResult);
    return error == std::errc{} && parsedEnd == end;
}

std::optional<taxbroker::Money> multiplyMoneyUnits(taxbroker::Money aPrice,
                                                   taxbroker::Units aUnits) {
    const auto exactProduct =
        taxbroker::ExactRational::create(taxbroker::WideInteger{aPrice} * aUnits,
                                         taxbroker::UNITS_SCALE);
    const auto roundedProductResult =
        taxbroker::roundToScaled(std::get<taxbroker::ExactRational>(exactProduct), 1);

    if (const auto* roundedProduct = std::get_if<taxbroker::Money>(&roundedProductResult))
    {
        return *roundedProduct;
    }

    return std::nullopt;
}

namespace numeric_detail {

std::optional<std::int64_t> parseScaledInt64(std::string_view aValue, std::int64_t aScale) {
    if (aScale <= 0)
        return std::nullopt;

    std::size_t precision = 0;
    for (auto remainingScale = aScale; remainingScale > 1; remainingScale /= 10)
    {
        if (remainingScale % 10 != 0)
            return std::nullopt;
        ++precision;
    }

    aValue = trim(aValue);
    if (aValue.empty())
        return std::nullopt;

    bool negative = false;
    if (aValue.front() == '(')
    {
        if (aValue.size() < 3 || aValue.back() != ')')
            return std::nullopt;
        negative = true;
        aValue.remove_prefix(1);
        aValue.remove_suffix(1);
    }
    else if (aValue.front() == '+' || aValue.front() == '-')
    {
        negative = aValue.front() == '-';
        aValue.remove_prefix(1);
    }

    if (aValue.empty())
        return std::nullopt;

    const auto decimalPoint = aValue.find('.');
    if (decimalPoint != std::string_view::npos &&
        aValue.find('.', decimalPoint + 1) != std::string_view::npos)
    {
        return std::nullopt;
    }

    const auto integerPart = aValue.substr(0, decimalPoint);
    const auto fractionalPart = decimalPoint == std::string_view::npos
                                    ? std::string_view{}
                                    : aValue.substr(decimalPoint + 1);
    if (integerPart.empty() && fractionalPart.empty())
        return std::nullopt;
    if (!hasValidIntegerGrouping(integerPart))
        return std::nullopt;

    const auto limit = signedMagnitudeLimit(negative);
    const auto integerLimit = limit / static_cast<std::uint64_t>(aScale);
    std::uint64_t integerMagnitude = 0;
    bool hasIntegerDigit = false;

    for (const char character : integerPart)
    {
        if (character == ',')
            continue;
        if (!std::isdigit(static_cast<unsigned char>(character)))
            return std::nullopt;

        hasIntegerDigit = true;
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (digit > integerLimit)
            return std::nullopt;
        if (integerMagnitude > (integerLimit - digit) / 10U)
            return std::nullopt;
        integerMagnitude = integerMagnitude * 10U + digit;
    }

    std::uint64_t fractionalMagnitude = 0;
    std::size_t fractionalDigits = 0;
    bool roundUp = false;
    for (const char character : fractionalPart)
    {
        if (!std::isdigit(static_cast<unsigned char>(character)))
            return std::nullopt;

        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (fractionalDigits < precision)
        {
            fractionalMagnitude = fractionalMagnitude * 10U + digit;
        }
        else if (fractionalDigits == precision)
        {
            roundUp = digit >= 5U;
        }
        ++fractionalDigits;
    }

    if (!hasIntegerDigit && fractionalPart.empty())
        return std::nullopt;
    while (fractionalDigits < precision)
    {
        fractionalMagnitude *= 10U;
        ++fractionalDigits;
    }

    std::uint64_t scaledMagnitude = integerMagnitude * static_cast<std::uint64_t>(aScale);
    if (fractionalMagnitude > limit - scaledMagnitude)
        return std::nullopt;
    scaledMagnitude += fractionalMagnitude;

    if (roundUp)
    {
        if (scaledMagnitude == limit)
            return std::nullopt;
        ++scaledMagnitude;
    }

    return applySign(scaledMagnitude, negative);
}

} // namespace numeric_detail
