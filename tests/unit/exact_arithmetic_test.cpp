#include "taxbroker/exact_arithmetic.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <variant>

namespace {
using namespace taxbroker;

template <typename T> T requireSuccessfulValue(const NumericResult<T>& aResult) {
    if (const auto* value = std::get_if<T>(&aResult))
    {
        return *value;
    }

    throw std::runtime_error{"Expected a successful synthetic arithmetic result"};
}

ExactRational makeExactFraction(WideInteger aNumerator, WideInteger aDenominator = 1) {
    return requireSuccessfulValue(
        ExactRational::create(std::move(aNumerator), std::move(aDenominator)));
}

template <typename T>
void expectNumericError(const NumericResult<T>& aResult, NumericError aError) {
    ASSERT_TRUE(std::holds_alternative<NumericError>(aResult));
    EXPECT_EQ(std::get<NumericError>(aResult), aError);
}

TEST(ExactArithmeticTest, RoundsDirectlyWithoutAnIntermediateDecimalScale) {
    const auto exact = makeExactFraction(1'234'500'499, 1'000'000'000);

    EXPECT_EQ(requireSuccessfulValue(roundToScaled(exact, 1'000'000)), 1'234'500);
    EXPECT_EQ(requireSuccessfulValue(
                  roundToScaled(makeExactFraction(-1'234'500'499, 1'000'000'000), 1'000'000)),
              -1'234'500);

    for (const auto sign : {1, -1})
    {
        EXPECT_EQ(requireSuccessfulValue(
                      roundToScaled(makeExactFraction(sign * 123'454, 100'000), MONEY_SCALE)),
                  sign * 12'345);
        EXPECT_EQ(requireSuccessfulValue(
                      roundToScaled(makeExactFraction(sign * 123'455, 100'000), MONEY_SCALE)),
                  sign * 12'346);
        EXPECT_EQ(requireSuccessfulValue(
                      roundToScaled(makeExactFraction(sign * 123'456, 100'000), MONEY_SCALE)),
                  sign * 12'346);
        EXPECT_EQ(requireSuccessfulValue(roundToScaled(makeExactFraction(sign, 2), 1)), sign);
        EXPECT_EQ(requireSuccessfulValue(roundToScaled(makeExactFraction(sign, 3), 1)), 0);
        EXPECT_EQ(requireSuccessfulValue(roundToScaled(makeExactFraction(sign * 2, 3), 1)), sign);
        EXPECT_EQ(requireSuccessfulValue(roundToScaled(makeExactFraction(sign, 200), 100)), sign);
        EXPECT_EQ(requireSuccessfulValue(
                      roundToScaled(makeExactFraction(sign, 200'000'000), UNITS_SCALE)),
                  sign);
    }
}

TEST(ExactArithmeticTest, ValidatesSignPositiveZeroAndDestinationRanges) {
    const DestinationRange positive{.mPolicy = ValuePolicy::Positive};
    const DestinationRange nonnegative{.mPolicy = ValuePolicy::Nonnegative};

    expectNumericError(roundToScaled(makeExactFraction(4, 100'000), MONEY_SCALE, positive),
                       NumericError::UnrepresentableValue);
    expectNumericError(roundToScaled(makeExactFraction(-1, 100'000), MONEY_SCALE, nonnegative),
                       NumericError::UnrepresentableValue);
    expectNumericError(roundToScaled(ExactRational{}, 1, positive),
                       NumericError::UnrepresentableValue);
    EXPECT_EQ(requireSuccessfulValue(roundToScaled(ExactRational{}, 1, nonnegative)), 0);
    expectNumericError(roundToScaled(makeExactFraction(11), 1, {0, 10}),
                       NumericError::UnrepresentableValue);
    expectNumericError(roundToScaled(makeExactFraction(1), 1, {10, 0}), NumericError::InvalidInput);
    expectNumericError(roundToScaled(makeExactFraction(1), 0), NumericError::InvalidInput);
}

TEST(ExactArithmeticTest, ConvertsEachMoneyValueInTheExplicitRateDirection) {
    EXPECT_EQ(requireSuccessfulValue(
                  convertMoney(1'234'567, 125'000'000, ExchangeRateDirection::ForeignPerEur)),
              987'654);
    EXPECT_EQ(requireSuccessfulValue(
                  convertMoney(100'000, 90'000'000, ExchangeRateDirection::EurPerForeign)),
              90'000);
    EXPECT_EQ(requireSuccessfulValue(
                  convertMoney(12'345, EXCHANGE_RATE_SCALE, ExchangeRateDirection::ForeignPerEur)),
              12'345);
    EXPECT_EQ(
        requireSuccessfulValue(convertMoney(1, 200'000'000, ExchangeRateDirection::ForeignPerEur)),
        1);
    EXPECT_EQ(
        requireSuccessfulValue(convertMoney(-1, 200'000'000, ExchangeRateDirection::ForeignPerEur)),
        -1);
    expectNumericError(convertMoney(1,
                                    300'000'000,
                                    ExchangeRateDirection::ForeignPerEur,
                                    {.mPolicy = ValuePolicy::Positive}),
                       NumericError::UnrepresentableValue);
    expectNumericError(convertMoney(1, 0, ExchangeRateDirection::ForeignPerEur),
                       NumericError::InvalidExchangeRate);
    expectNumericError(convertMoney(1, -1, ExchangeRateDirection::EurPerForeign),
                       NumericError::InvalidExchangeRate);
    expectNumericError(convertMoney(1, 1, static_cast<ExchangeRateDirection>(99)),
                       NumericError::InvalidExchangeRate);
}

TEST(ExactArithmeticTest, ChecksIntegerEndpointsAndRoundingCarry) {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();

    EXPECT_EQ(requireSuccessfulValue(checkedAdd(maximum, 0)), maximum);
    EXPECT_EQ(requireSuccessfulValue(checkedSubtract(minimum, 0)), minimum);
    EXPECT_EQ(requireSuccessfulValue(checkedMagnitude(-12)), 12);
    EXPECT_EQ(requireSuccessfulValue(roundToScaled(makeExactFraction(minimum), 1)), minimum);
    EXPECT_EQ(requireSuccessfulValue(roundToScaled(makeExactFraction(maximum), 1)), maximum);
    expectNumericError(checkedAdd(maximum, 1), NumericError::Overflow);
    expectNumericError(checkedSubtract(minimum, 1), NumericError::Overflow);
    expectNumericError(checkedMagnitude(minimum), NumericError::Overflow);
    expectNumericError(roundToScaled(makeExactFraction(WideInteger{maximum} * 2 + 1, 2), 1),
                       NumericError::Overflow);
    expectNumericError(roundToScaled(makeExactFraction(WideInteger{minimum} * 2 - 1, 2), 1),
                       NumericError::Overflow);
    expectNumericError(convertMoney(maximum, 1, ExchangeRateDirection::ForeignPerEur),
                       NumericError::Overflow);
    expectNumericError(convertMoney(maximum, maximum, ExchangeRateDirection::EurPerForeign),
                       NumericError::Overflow);
}

TEST(ExactArithmeticTest, ReducesFractionsBeforeProductsAndComparesWithoutCrossProducts) {
    const WideInteger largeValue = WideInteger{1} << 200;
    const auto left = makeExactFraction(largeValue, 3);
    const auto right = makeExactFraction(3, largeValue);

    EXPECT_EQ(requireSuccessfulValue(multiplyExact(left, right)), makeExactFraction(1));
    EXPECT_EQ(requireSuccessfulValue(divideExact(left, left)), makeExactFraction(1));
    EXPECT_EQ(requireSuccessfulValue(addExact(makeExactFraction(largeValue - 1, largeValue),
                                              makeExactFraction(1, largeValue))),
              makeExactFraction(1));
    EXPECT_EQ(requireSuccessfulValue(subtractExact(left, left)), makeExactFraction(0));
    EXPECT_EQ(makeExactFraction(2, -4), makeExactFraction(-1, 2));
    EXPECT_EQ(makeExactFraction(0, -99), makeExactFraction(0));
    EXPECT_LT(compareExact(makeExactFraction(largeValue - 1, largeValue),
                           makeExactFraction(largeValue, largeValue + 1)),
              0);
    EXPECT_GT(compareExact(makeExactFraction(-1, 3), makeExactFraction(-1, 2)), 0);
    EXPECT_LT(compareExact(makeExactFraction(-1), makeExactFraction(0)), 0);
    EXPECT_EQ(compareExact(left, left), 0);
    expectNumericError(multiplyExact(left, left), NumericError::Overflow);
    expectNumericError(
        addExact(makeExactFraction(std::numeric_limits<WideInteger>::max()), makeExactFraction(1)),
        NumericError::Overflow);
    expectNumericError(divideExact(left, makeExactFraction(0)), NumericError::InvalidDenominator);
    expectNumericError(ExactRational::create(1, 0), NumericError::InvalidDenominator);
    expectNumericError(
        roundToScaled(makeExactFraction(std::numeric_limits<WideInteger>::max()), 100),
        NumericError::Overflow);
    EXPECT_EQ(left, makeExactFraction(largeValue, 3));
}

TEST(LargestRemainderTest, AdjustsCombinedUnitsAndReturnsTheExactSubstepResidual) {
    const std::array lots{UnitAllocationPart{UNITS_SCALE, 0},
                          UnitAllocationPart{2 * UNITS_SCALE, 1}};
    const auto adjustedAllocation = requireSuccessfulValue(allocateAdjustedUnits(lots, 5, 3));

    EXPECT_EQ(adjustedAllocation.mParts, (std::vector<std::int64_t>{166'666'667, 333'333'333}));
    EXPECT_EQ(adjustedAllocation.mTotal, 5 * UNITS_SCALE);
    EXPECT_EQ(adjustedAllocation.mResidual, makeExactFraction(0));
    const std::array oneLot{UnitAllocationPart{UNITS_SCALE, 0}};
    const auto oneThirdAllocation = requireSuccessfulValue(allocateAdjustedUnits(oneLot, 1, 3));

    EXPECT_EQ(oneThirdAllocation.mTotal, 33'333'333);
    EXPECT_EQ(oneThirdAllocation.mResidual, makeExactFraction(1, 300'000'000));
}

TEST(LargestRemainderTest, BreaksTiesByLotThenSaleOrderAndPreservesCallerOrder) {
    std::array parts{
        ExactAllocationPart{makeExactFraction(1, 30'000), {2, 0}},
        ExactAllocationPart{makeExactFraction(1, 30'000), {1, 2}},
        ExactAllocationPart{makeExactFraction(1, 30'000), {1, 1}},
    };
    const auto moneyAllocation = requireSuccessfulValue(allocateMoneyParts(parts));

    EXPECT_EQ(moneyAllocation.mParts, (std::vector<std::int64_t>{0, 0, 1}));
    EXPECT_EQ(moneyAllocation.mTotal, 1);
    std::reverse(parts.begin(), parts.end());
    EXPECT_EQ(requireSuccessfulValue(allocateMoneyParts(parts)).mParts,
              (std::vector<std::int64_t>{1, 0, 0}));
    const std::array unitLots{UnitAllocationPart{1, 5}, UnitAllocationPart{1, 2}};

    EXPECT_EQ(requireSuccessfulValue(allocateAdjustedUnits(unitLots, 3, 2)).mParts,
              (std::vector<std::int64_t>{1, 2}));
}

TEST(LargestRemainderTest, ComparesUnequalDenominatorsAndChecksInvalidAllocations) {
    const std::array parts{ExactAllocationPart{makeExactFraction(1, 15'000), {1, 0}},
                           ExactAllocationPart{makeExactFraction(1, 30'000), {0, 0}}};
    const auto moneyAllocation = requireSuccessfulValue(allocateMoneyParts(parts));

    EXPECT_EQ(moneyAllocation.mParts, (std::vector<std::int64_t>{1, 0}));
    EXPECT_EQ(std::accumulate(moneyAllocation.mParts.begin(),
                              moneyAllocation.mParts.end(),
                              std::int64_t{}),
              moneyAllocation.mTotal);
    EXPECT_EQ(requireSuccessfulValue(allocateMoneyParts({})).mTotal, 0);
    expectNumericError(allocateLargestRemainder(parts, 0), NumericError::InvalidInput);
    const std::array duplicateKeys{parts[0], parts[0]};
    const std::array negativePart{ExactAllocationPart{makeExactFraction(-1), {0, 0}}};
    const std::array largeValue{
        ExactAllocationPart{makeExactFraction(std::numeric_limits<std::int64_t>::max()), {0, 0}}};

    expectNumericError(allocateMoneyParts(duplicateKeys), NumericError::InvalidInput);
    expectNumericError(allocateMoneyParts(negativePart), NumericError::UnrepresentableValue);
    expectNumericError(allocateMoneyParts(largeValue), NumericError::Overflow);
    const std::array smallUnitLots{UnitAllocationPart{1, 0}, UnitAllocationPart{1, 1}};

    expectNumericError(allocateAdjustedUnits(smallUnitLots, 1, 3),
                       NumericError::UnrepresentableValue);
    expectNumericError(allocateAdjustedUnits(smallUnitLots, 0, 1), NumericError::InvalidInput);
    expectNumericError(allocateAdjustedUnits(smallUnitLots, 1, 1), NumericError::InvalidInput);
    const std::array zeroUnitLot{UnitAllocationPart{0, 0}};

    expectNumericError(allocateAdjustedUnits(zeroUnitLot, 2, 1),
                       NumericError::UnrepresentableValue);
}

} // namespace
