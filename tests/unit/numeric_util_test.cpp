#include "taxbroker/types.hpp"
#include "utils/numeric_util.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace {

using taxbroker::Money;
using taxbroker::MONEY_SCALE;
using taxbroker::Units;
using taxbroker::UNITS_SCALE;

std::optional<Money> parseMoney(std::string_view aValue) {
    return parseScaledNumber<Money, MONEY_SCALE>(aValue);
}

std::optional<Units> parseUnits(std::string_view aValue) {
    return parseScaledNumber<Units, UNITS_SCALE>(aValue);
}

TEST(ParseScaledNumberTest, ParsesTradeRepublicValuesExactly) {
    EXPECT_EQ(parseMoney("204.300000"), 2'043'000);
    EXPECT_EQ(parseMoney("0.070000"), 700);
    EXPECT_EQ(parseMoney("-0.01"), -100);
    EXPECT_EQ(parseUnits("0.2376590000"), 23'765'900);
    EXPECT_EQ(parseUnits("-0.2376590000"), -23'765'900);
}

TEST(ParseScaledNumberTest, RoundsExcessPrecisionAwayFromZeroAtHalf) {
    EXPECT_EQ(parseMoney("1.23454"), 12'345);
    EXPECT_EQ(parseMoney("1.23455"), 12'346);
    EXPECT_EQ(parseMoney("-1.23455"), -12'346);
    EXPECT_EQ(parseMoney("9.99999"), 100'000);
}

TEST(ParseScaledNumberTest, SupportsWhitespaceGroupingAndParenthesizedNegativeValues) {
    EXPECT_EQ(parseMoney(" 1,234.50 "), 12'345'000);
    EXPECT_EQ(parseMoney("(12.34)"), -123'400);
    EXPECT_EQ(parseMoney(".5"), 5'000);
}

TEST(ParseScaledNumberTest, RejectsMalformedAndOutOfRangeValues) {
    EXPECT_FALSE(parseMoney("").has_value());
    EXPECT_FALSE(parseMoney("abc").has_value());
    EXPECT_FALSE(parseMoney("1.2.3").has_value());
    EXPECT_FALSE(parseMoney("12,34.50").has_value());
    EXPECT_FALSE(parseMoney("922337203685477.5808").has_value());
}

TEST(ParseScaledNumberTest, AcceptsSignedInt64Boundaries) {
    EXPECT_EQ(parseMoney("922337203685477.5807"), std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(parseMoney("-922337203685477.5808"), std::numeric_limits<std::int64_t>::min());
}

TEST(MultiplyMoneyUnitsTest, ProducesMoneyWithExplicitRounding) {
    EXPECT_EQ(multiplyMoneyUnits(2'043'000, 14'760'000), 301'547);
    EXPECT_EQ(multiplyMoneyUnits(-2'043'000, 14'760'000), -301'547);
    EXPECT_EQ(multiplyMoneyUnits(1, 50'000'000), 1);
    EXPECT_EQ(multiplyMoneyUnits(-1, 50'000'000), -1);
}

TEST(MultiplyMoneyUnitsTest, HandlesBoundariesAndReportsOverflow) {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();

    EXPECT_EQ(multiplyMoneyUnits(maximum, UNITS_SCALE), maximum);
    EXPECT_EQ(multiplyMoneyUnits(minimum, UNITS_SCALE), minimum);
    EXPECT_FALSE(multiplyMoneyUnits(maximum, UNITS_SCALE + 1).has_value());
    EXPECT_FALSE(multiplyMoneyUnits(minimum, -UNITS_SCALE).has_value());
}

} // namespace
