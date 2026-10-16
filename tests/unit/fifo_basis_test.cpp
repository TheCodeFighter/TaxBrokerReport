#include "taxbroker/fifo_basis.hpp"

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

template <typename T> T requireValue(const NumericResult<T>& aResult) {
    if (const auto* value = std::get_if<T>(&aResult))
    {
        return *value;
    }

    throw std::runtime_error{"Expected a successful synthetic arithmetic result"};
}

ExactRational fraction(WideInteger aNumerator, WideInteger aDenominator = 1) {
    return requireValue(ExactRational::create(std::move(aNumerator), std::move(aDenominator)));
}

template <typename T> void expectError(const NumericResult<T>& aResult, NumericError aError) {
    ASSERT_TRUE(std::holds_alternative<NumericError>(aResult));
    EXPECT_EQ(std::get<NumericError>(aResult), aError);
}

TEST(FifoBasisTest, PreservesTheForeignPurchaseSplitAndPartialDisposalExample) {
    const auto eurPrice =
        requireValue(convertMoney(2'553'750, 125'000'000, ExchangeRateDirection::ForeignPerEur));
    const auto acquired = requireValue(createLotBasis(eurPrice, 14'760'000));
    const auto adjusted = requireValue(adjustLotUnits(acquired, 29'520'000));
    const auto first = requireValue(consumeLotBasis(adjusted, 10'000'000));
    const auto final = requireValue(consumeLotBasis(first.mRemaining, 19'520'000));

    EXPECT_EQ(eurPrice, 2'043'000);
    EXPECT_EQ(acquired.mRemainingBasis.mTicks, fraction(30'154'680'000'000));
    EXPECT_EQ(adjusted.mRemainingBasis, acquired.mRemainingBasis);
    EXPECT_EQ(requireValue(exactLotUnitValue(adjusted)), fraction(10'215, 100));
    EXPECT_EQ(first.mAllocatedBasis.mTicks, fraction(10'215'000'000'000));
    EXPECT_EQ(first.mRemaining.mRemainingBasis.mTicks, fraction(19'939'680'000'000));
    EXPECT_EQ(final.mAllocatedBasis, first.mRemaining.mRemainingBasis);
    EXPECT_EQ(final.mRemaining, LotBasisState{});

    const std::array parts{
        ExactAllocationPart{requireValue(basisInEur(first.mAllocatedBasis)), {0, 0}},
        ExactAllocationPart{requireValue(basisInEur(final.mAllocatedBasis)), {0, 1}},
    };
    const auto emitted = requireValue(allocateMoneyParts(parts));

    EXPECT_EQ(emitted.mParts, (std::vector<std::int64_t>{102'150, 199'397}));
    EXPECT_EQ(emitted.mTotal, 301'547);
    EXPECT_EQ(acquired.mRemainingBasis.mTicks, fraction(30'154'680'000'000));
}

TEST(FifoBasisTest, AllocatesExactNonterminatingCostsAcrossRepeatedSalesAfterOneAdjustment) {
    const auto original = requireValue(createLotBasis(1, 3));
    const auto adjusted = requireValue(adjustLotUnits(original, 7));

    EXPECT_EQ(original.mRemainingBasis.mTicks, fraction(3));
    EXPECT_EQ(adjusted.mRemainingUnits, 7);
    EXPECT_EQ(adjusted.mRemainingBasis.mTicks, fraction(3));

    // Three cost ticks (10^-12 EUR each) across seven quantity ticks give 3/7 cost ticks
    // per quantity tick. These fractions stay exact; no rounding applies between sales.
    const auto first = requireValue(consumeLotBasis(adjusted, 1));
    auto allocated = first.mAllocatedBasis.mTicks;

    EXPECT_EQ(first.mAllocatedBasis.mTicks, fraction(3, 7));
    EXPECT_EQ(first.mRemaining.mRemainingUnits, 6);
    EXPECT_EQ(first.mRemaining.mRemainingBasis.mTicks, fraction(18, 7));
    EXPECT_EQ(requireValue(addExact(allocated, first.mRemaining.mRemainingBasis.mTicks)),
              original.mRemainingBasis.mTicks);

    const auto second = requireValue(consumeLotBasis(first.mRemaining, 2));
    allocated = requireValue(addExact(allocated, second.mAllocatedBasis.mTicks));

    EXPECT_EQ(second.mAllocatedBasis.mTicks, fraction(6, 7));
    EXPECT_EQ(second.mRemaining.mRemainingUnits, 4);
    EXPECT_EQ(second.mRemaining.mRemainingBasis.mTicks, fraction(12, 7));
    EXPECT_EQ(requireValue(addExact(allocated, second.mRemaining.mRemainingBasis.mTicks)),
              original.mRemainingBasis.mTicks);

    const auto third = requireValue(consumeLotBasis(second.mRemaining, 1));
    allocated = requireValue(addExact(allocated, third.mAllocatedBasis.mTicks));

    EXPECT_EQ(third.mAllocatedBasis.mTicks, fraction(3, 7));
    EXPECT_EQ(third.mRemaining.mRemainingUnits, 3);
    EXPECT_EQ(third.mRemaining.mRemainingBasis.mTicks, fraction(9, 7));
    EXPECT_EQ(requireValue(addExact(allocated, third.mRemaining.mRemainingBasis.mTicks)),
              original.mRemainingBasis.mTicks);

    const auto final = requireValue(consumeLotBasis(third.mRemaining, 3));
    allocated = requireValue(addExact(allocated, final.mAllocatedBasis.mTicks));

    EXPECT_EQ(final.mAllocatedBasis.mTicks, fraction(9, 7));
    EXPECT_EQ(final.mRemaining.mRemainingUnits, 0);
    EXPECT_EQ(final.mRemaining.mRemainingBasis.mTicks, fraction(0));
    EXPECT_EQ(requireValue(addExact(allocated, final.mRemaining.mRemainingBasis.mTicks)),
              original.mRemainingBasis.mTicks);
    EXPECT_EQ(final.mRemaining, LotBasisState{});
    EXPECT_EQ(allocated, fraction(3));
}

TEST(FifoBasisTest, SupportsProductsBeyond128BitsAndLeavesFailedStateUnchanged) {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto lot = requireValue(createLotBasis(maximum, maximum));
    const auto sale = requireValue(consumeLotBasis(lot, maximum - 1));

    EXPECT_EQ(
        requireValue(addExact(sale.mAllocatedBasis.mTicks, sale.mRemaining.mRemainingBasis.mTicks)),
        lot.mRemainingBasis.mTicks);
    const auto adjusted = requireValue(adjustLotUnits(lot, maximum - 2));
    const auto adjustedSale = requireValue(consumeLotBasis(adjusted, maximum - 3));

    EXPECT_GT(adjustedSale.mAllocatedBasis.mTicks.numerator(), WideInteger{1} << 128);
    const auto snapshot = lot;

    expectError(consumeLotBasis(lot, -1), NumericError::UnrepresentableValue);
    expectError(consumeLotBasis(lot, 0), NumericError::UnrepresentableValue);
    expectError(consumeLotBasis(LotBasisState{}, 1), NumericError::UnrepresentableValue);
    expectError(consumeLotBasis(requireValue(createLotBasis(1, 1)), 2),
                NumericError::UnrepresentableValue);
    expectError(adjustLotUnits(lot, 0), NumericError::UnrepresentableValue);
    expectError(createLotBasis(0, 1), NumericError::UnrepresentableValue);
    expectError(exactLotUnitValue(LotBasisState{}), NumericError::UnrepresentableValue);
    EXPECT_EQ(lot, snapshot);
    const LotBasisState wideLot{std::numeric_limits<Units>::max(),
                                ExactBasis{fraction(WideInteger{1} << 250)}};
    const auto wideSnapshot = wideLot;

    expectError(consumeLotBasis(wideLot, wideLot.mRemainingUnits - 1), NumericError::Overflow);
    EXPECT_EQ(wideLot, wideSnapshot);
}

TEST(FifoBasisTest, PreservesBasisWithLargestRemainderAdjustedUnits) {
    const std::array lots{UnitAllocationPart{UNITS_SCALE, 0},
                          UnitAllocationPart{2 * UNITS_SCALE, 1}};
    const auto adjusted = requireValue(allocateAdjustedUnits(lots, 5, 3));

    const auto firstLot = requireValue(
        adjustLotUnits(requireValue(createLotBasis(100'000, UNITS_SCALE)), adjusted.mParts[0]));
    const auto secondLot = requireValue(
        adjustLotUnits(requireValue(createLotBasis(100'000, 2 * UNITS_SCALE)), adjusted.mParts[1]));

    EXPECT_EQ(requireValue(roundToScaled(requireValue(exactLotUnitValue(firstLot)), UNITS_SCALE)),
              599'999'999);
    EXPECT_EQ(requireValue(roundToScaled(requireValue(exactLotUnitValue(secondLot)), UNITS_SCALE)),
              600'000'001);
    EXPECT_EQ(firstLot.mRemainingBasis.mTicks, fraction(10'000'000'000'000));
    EXPECT_EQ(secondLot.mRemainingBasis.mTicks, fraction(20'000'000'000'000));
}

} // namespace
