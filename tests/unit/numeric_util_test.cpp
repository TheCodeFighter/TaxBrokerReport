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

TEST(ParseIntegerTest, ParsesOnlyCompleteIntegers) {
    int value{};
    EXPECT_TRUE(parseInteger("2024", value));
    EXPECT_EQ(value, 2024);
    EXPECT_TRUE(parseInteger("-12", value));
    EXPECT_EQ(value, -12);
    EXPECT_FALSE(parseInteger("", value));
    EXPECT_FALSE(parseInteger("12x", value));
    EXPECT_FALSE(parseInteger("2147483648", value));
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

TEST(ParseScaledNumberTest, SupportsWhitespaceAndLeadingDecimalPoint) {
    EXPECT_EQ(parseMoney(" 1234.50 "), 12'345'000);
    EXPECT_EQ(parseMoney(".5"), 5'000);
}

TEST(ParseScaledNumberTest, RejectsDigitGroupingDecimalCommasAndParentheses) {
    EXPECT_FALSE(parseMoney("1,234.50").has_value());
    EXPECT_FALSE(parseMoney("(12.34)").has_value());
    EXPECT_FALSE(parseUnits("0,125").has_value());
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

TEST(MultiplyUnitPriceUnitsTest, ProducesMoneyWithExplicitRounding) {
    // EUR 204.3 * 0.1476 units = 30.15468, rounded to Money 30.1547.
    EXPECT_EQ(multiplyUnitPriceUnits(20'430'000'000, 14'760'000), 301'547);
    EXPECT_EQ(multiplyUnitPriceUnits(-20'430'000'000, 14'760'000), -301'547);

    // EUR 0.0001 * 0.5 units = 0.00005, exactly half of one Money step: away from zero.
    EXPECT_EQ(multiplyUnitPriceUnits(10'000, 50'000'000), 1);
    EXPECT_EQ(multiplyUnitPriceUnits(-10'000, 50'000'000), -1);

    // EUR 0.00004999 * 1 unit is below half of one Money step.
    EXPECT_EQ(multiplyUnitPriceUnits(4'999, 100'000'000), 0);
}

TEST(MultiplyUnitPriceUnitsTest, KeepsTheEightDecimalPriceForLargeQuantities) {
    // EUR 10.00005 * 1,000,000 units = EUR 10,000,050.0000. At four decimals the price would
    // be 10.0001, which gives EUR 10,000,100.0000.
    EXPECT_EQ(multiplyUnitPriceUnits(1'000'005'000, 100'000'000'000'000), 100'000'500'000);
}

TEST(MultiplyUnitPriceUnitsTest, HandlesBoundariesAndReportsOverflow) {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    // 10,000 units at eight decimals: the product / 10^12 returns the price unchanged as Money.
    constexpr auto tenThousandUnits = 10'000 * UNITS_SCALE;

    EXPECT_EQ(multiplyUnitPriceUnits(maximum, tenThousandUnits), maximum);
    EXPECT_EQ(multiplyUnitPriceUnits(minimum, tenThousandUnits), minimum);
    EXPECT_FALSE(multiplyUnitPriceUnits(maximum, tenThousandUnits + 1).has_value());
    EXPECT_FALSE(multiplyUnitPriceUnits(minimum, -tenThousandUnits).has_value());

    // The largest price times 1 unit is EUR 92233720368.54775807, which rounds to Money
    // 92233720368.5478 (the digits after the fourth decimal, 5807, round up).
    EXPECT_EQ(multiplyUnitPriceUnits(maximum, UNITS_SCALE), 922'337'203'685'478);
}

} // namespace
