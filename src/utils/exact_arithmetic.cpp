#include "taxbroker/exact_arithmetic.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace taxbroker {
namespace {

WideInteger absoluteValue(const WideInteger& aValue) {
    return aValue < 0 ? -aValue : aValue;
}

/// Euclid's remainder algorithm finds the common factor used to reduce exact fractions.
WideInteger greatestCommonDivisor(WideInteger aLeft, WideInteger aRight) {
    aLeft = absoluteValue(aLeft);
    aRight = absoluteValue(aRight);

    while (aRight != 0)
    {
        WideInteger remainder = aLeft % aRight;
        aLeft = std::move(aRight);
        aRight = std::move(remainder);
    }

    return aLeft;
}

/// Converts checked-integer library exceptions into the numeric failure contract.
template <typename T, typename Operation>
NumericResult<T> withOverflowCheck(const Operation& aOperation) {
    try
    { return aOperation(); } catch (const std::overflow_error&)
    { return NumericError::Overflow; } catch (const std::range_error&)
    { return NumericError::Overflow; }
}

/// Checks the wide value before conversion so it cannot truncate or wrap in int64_t storage.
NumericResult<std::int64_t> checkedInt64(const WideInteger& aValue) {
    if (aValue < std::numeric_limits<std::int64_t>::min() ||
        aValue > std::numeric_limits<std::int64_t>::max())
    {
        return NumericError::Overflow;
    }

    return aValue.convert_to<std::int64_t>();
}

/// Continued fractions compare without multiplying potentially large denominators.
int compareNonnegativeFractions(WideInteger aLeftNumerator,
                                WideInteger aLeftDenominator,
                                WideInteger aRightNumerator,
                                WideInteger aRightDenominator) {
    int comparisonDirection = 1;

    for (;;)
    {
        const WideInteger leftQuotient = aLeftNumerator / aLeftDenominator;
        const WideInteger rightQuotient = aRightNumerator / aRightDenominator;

        if (leftQuotient != rightQuotient)
        {
            return comparisonDirection * (leftQuotient < rightQuotient ? -1 : 1);
        }

        WideInteger leftRemainder = aLeftNumerator % aLeftDenominator;
        WideInteger rightRemainder = aRightNumerator % aRightDenominator;

        if (leftRemainder == 0 || rightRemainder == 0)
        {
            return comparisonDirection * (leftRemainder == rightRemainder ? 0
                                          : leftRemainder == 0            ? -1
                                                                          : 1);
        }

        aLeftNumerator = std::move(aLeftDenominator);
        aLeftDenominator = std::move(leftRemainder);
        aRightNumerator = std::move(aRightDenominator);
        aRightDenominator = std::move(rightRemainder);
        comparisonDirection = -comparisonDirection;
    }
}

/// Aligns denominators using their common divisor, then cancels again before the final product.
NumericResult<ExactRational>
combineExactFractions(const ExactRational& aLeft, const ExactRational& aRight, bool aSubtract) {
    return withOverflowCheck<ExactRational>([&]() -> NumericResult<ExactRational> {
        const WideInteger commonDivisor =
            greatestCommonDivisor(aLeft.denominator(), aRight.denominator());
        const WideInteger leftNumeratorMultiplier = aRight.denominator() / commonDivisor;
        const WideInteger rightNumeratorMultiplier = aLeft.denominator() / commonDivisor;
        const WideInteger leftNumerator = aLeft.numerator() * leftNumeratorMultiplier;
        const WideInteger rightNumerator = aRight.numerator() * rightNumeratorMultiplier;
        const WideInteger numerator =
            aSubtract ? leftNumerator - rightNumerator : leftNumerator + rightNumerator;
        const WideInteger reductionDivisor = greatestCommonDivisor(numerator, commonDivisor);

        return ExactRational::create(numerator / reductionDivisor,
                                     rightNumeratorMultiplier *
                                         (aRight.denominator() / reductionDivisor));
    });
}

} // namespace

ExactRational::ExactRational(WideInteger aInteger) : mNumerator(std::move(aInteger)) {}

ExactRational::ExactRational(WideInteger aNumerator, WideInteger aDenominator)
    : mNumerator(std::move(aNumerator)), mDenominator(std::move(aDenominator)) {}

NumericResult<ExactRational> ExactRational::create(WideInteger aNumerator,
                                                   WideInteger aDenominator) {
    if (aDenominator == 0)
    {
        return NumericError::InvalidDenominator;
    }

    return withOverflowCheck<ExactRational>([&]() -> NumericResult<ExactRational> {
        if (aDenominator < 0)
        {
            aNumerator = -aNumerator;
            aDenominator = -aDenominator;
        }

        const WideInteger commonDivisor = greatestCommonDivisor(aNumerator, aDenominator);

        return ExactRational{aNumerator / commonDivisor, aDenominator / commonDivisor};
    });
}

const WideInteger& ExactRational::numerator() const noexcept {
    return mNumerator;
}

const WideInteger& ExactRational::denominator() const noexcept {
    return mDenominator;
}

NumericResult<std::int64_t> checkedAdd(std::int64_t aLeft, std::int64_t aRight) {
    return checkedInt64(WideInteger{aLeft} + aRight);
}

NumericResult<std::int64_t> checkedSubtract(std::int64_t aLeft, std::int64_t aRight) {
    return checkedInt64(WideInteger{aLeft} - aRight);
}

NumericResult<std::int64_t> checkedMagnitude(std::int64_t aValue) {
    return checkedInt64(absoluteValue(WideInteger{aValue}));
}

NumericResult<std::int64_t> validateDestination(std::int64_t aValue, DestinationRange aRange) {
    if (aRange.mMinimum > aRange.mMaximum)
    {
        return NumericError::InvalidInput;
    }

    if (aValue < aRange.mMinimum || aValue > aRange.mMaximum ||
        (aRange.mPolicy == ValuePolicy::Nonnegative && aValue < 0) ||
        (aRange.mPolicy == ValuePolicy::Positive && aValue <= 0))
    {
        return NumericError::UnrepresentableValue;
    }

    return aValue;
}

NumericResult<ExactRational> addExact(const ExactRational& aLeft, const ExactRational& aRight) {
    return combineExactFractions(aLeft, aRight, false);
}

NumericResult<ExactRational> subtractExact(const ExactRational& aLeft,
                                           const ExactRational& aRight) {
    return combineExactFractions(aLeft, aRight, true);
}

NumericResult<ExactRational> multiplyExact(const ExactRational& aLeft,
                                           const ExactRational& aRight) {
    return withOverflowCheck<ExactRational>([&]() -> NumericResult<ExactRational> {
        const WideInteger leftCancellationDivisor =
            greatestCommonDivisor(aLeft.numerator(), aRight.denominator());
        const WideInteger rightCancellationDivisor =
            greatestCommonDivisor(aRight.numerator(), aLeft.denominator());

        return ExactRational::create((aLeft.numerator() / leftCancellationDivisor) *
                                         (aRight.numerator() / rightCancellationDivisor),
                                     (aLeft.denominator() / rightCancellationDivisor) *
                                         (aRight.denominator() / leftCancellationDivisor));
    });
}

NumericResult<ExactRational> divideExact(const ExactRational& aLeft, const ExactRational& aRight) {
    if (aRight.numerator() == 0)
    {
        return NumericError::InvalidDenominator;
    }

    const auto reciprocal = ExactRational::create(aRight.denominator(), aRight.numerator());

    if (const auto* error = std::get_if<NumericError>(&reciprocal))
    {
        return *error;
    }

    return multiplyExact(aLeft, std::get<ExactRational>(reciprocal));
}

int compareExact(const ExactRational& aLeft, const ExactRational& aRight) {
    const bool leftNegative = aLeft.numerator() < 0;
    const bool rightNegative = aRight.numerator() < 0;

    if (leftNegative != rightNegative)
    {
        return leftNegative ? -1 : 1;
    }

    const int comparison = compareNonnegativeFractions(absoluteValue(aLeft.numerator()),
                                                       aLeft.denominator(),
                                                       absoluteValue(aRight.numerator()),
                                                       aRight.denominator());

    return leftNegative ? -comparison : comparison;
}

NumericResult<std::int64_t>
roundToScaled(const ExactRational& aValue, std::int64_t aScale, DestinationRange aRange) {
    if (aScale <= 0)
    {
        return NumericError::InvalidInput;
    }

    if ((aRange.mPolicy != ValuePolicy::Signed && aValue.numerator() < 0) ||
        (aRange.mPolicy == ValuePolicy::Positive && aValue.numerator() == 0))
    {
        return NumericError::UnrepresentableValue;
    }

    return withOverflowCheck<std::int64_t>([&]() -> NumericResult<std::int64_t> {
        const WideInteger commonDivisor =
            greatestCommonDivisor(WideInteger{aScale}, aValue.denominator());
        const WideInteger reducedDenominator = aValue.denominator() / commonDivisor;
        const WideInteger scaledNumeratorMagnitude =
            absoluteValue(aValue.numerator()) * (aScale / commonDivisor);
        WideInteger roundedMagnitude = scaledNumeratorMagnitude / reducedDenominator;
        const WideInteger fractionalRemainder = scaledNumeratorMagnitude % reducedDenominator;

        // Equivalent to twice the remainder reaching the denominator, without doubling overflow.
        if (fractionalRemainder >= reducedDenominator - fractionalRemainder)
        {
            ++roundedMagnitude;
        }

        const auto roundedIntegerResult =
            checkedInt64(aValue.numerator() < 0 ? -roundedMagnitude : roundedMagnitude);

        if (const auto* error = std::get_if<NumericError>(&roundedIntegerResult))
        {
            return *error;
        }

        return validateDestination(std::get<std::int64_t>(roundedIntegerResult), aRange);
    });
}

NumericResult<Money> convertMoney(Money aForeignMoney,
                                  ExchangeRate aRate,
                                  ExchangeRateDirection aDirection,
                                  DestinationRange aRange) {
    if (aRate <= 0)
    {
        return NumericError::InvalidExchangeRate;
    }

    NumericResult<ExactRational> converted = NumericError::InvalidExchangeRate;

    switch (aDirection)
    {
    case ExchangeRateDirection::ForeignPerEur:
        converted = ExactRational::create(WideInteger{aForeignMoney} * EXCHANGE_RATE_SCALE, aRate);
        break;
    case ExchangeRateDirection::EurPerForeign:
        converted = ExactRational::create(WideInteger{aForeignMoney} * aRate, EXCHANGE_RATE_SCALE);
        break;
    }

    if (const auto* error = std::get_if<NumericError>(&converted))
    {
        return *error;
    }

    return roundToScaled(std::get<ExactRational>(converted), 1, aRange);
}

NumericResult<RoundedAllocation>
allocateLargestRemainder(std::span<const ExactAllocationPart> aParts, std::int64_t aScale) {
    if (aScale <= 0)
    {
        return NumericError::InvalidInput;
    }

    return withOverflowCheck<RoundedAllocation>([&]() -> NumericResult<RoundedAllocation> {
        ExactRational exactTotal;
        WideInteger sumOfFlooredParts = 0;
        RoundedAllocation allocation;
        std::vector<ExactRational> fractionalRemainders;
        std::vector<std::size_t> partIndicesInPriorityOrder(aParts.size());
        std::iota(partIndicesInPriorityOrder.begin(), partIndicesInPriorityOrder.end(), 0);
        std::sort(partIndicesInPriorityOrder.begin(),
                  partIndicesInPriorityOrder.end(),
                  [&](std::size_t aLeft, std::size_t aRight) {
                      return aParts[aLeft].mKey < aParts[aRight].mKey;
                  });

        for (std::size_t index = 1; index < partIndicesInPriorityOrder.size(); ++index)
        {
            if (aParts[partIndicesInPriorityOrder[index - 1]].mKey ==
                aParts[partIndicesInPriorityOrder[index]].mKey)
            {
                return NumericError::InvalidInput;
            }
        }

        // Sum in key order so checked range behavior is independent of caller permutation.
        for (const auto index : partIndicesInPriorityOrder)
        {
            const auto& part = aParts[index];

            if (part.mValue.numerator() < 0)
            {
                return NumericError::UnrepresentableValue;
            }

            const auto updatedTotalResult = addExact(exactTotal, part.mValue);

            if (const auto* error = std::get_if<NumericError>(&updatedTotalResult))
            {
                return *error;
            }

            exactTotal = std::get<ExactRational>(updatedTotalResult);
        }

        const auto roundedTotalResult = roundToScaled(exactTotal, aScale);

        if (const auto* error = std::get_if<NumericError>(&roundedTotalResult))
        {
            return *error;
        }

        allocation.mTotal = std::get<std::int64_t>(roundedTotalResult);

        for (const auto& part : aParts)
        {
            const auto scaledPartResult = multiplyExact(part.mValue, ExactRational{aScale});

            if (const auto* error = std::get_if<NumericError>(&scaledPartResult))
            {
                return *error;
            }

            const auto& scaledPart = std::get<ExactRational>(scaledPartResult);
            const WideInteger flooredPart = scaledPart.numerator() / scaledPart.denominator();
            const auto flooredPartResult = checkedInt64(flooredPart);

            if (const auto* error = std::get_if<NumericError>(&flooredPartResult))
            {
                return *error;
            }

            sumOfFlooredParts += flooredPart;
            allocation.mParts.push_back(std::get<std::int64_t>(flooredPartResult));
            fractionalRemainders.push_back(std::get<ExactRational>(
                ExactRational::create(scaledPart.numerator() % scaledPart.denominator(),
                                      scaledPart.denominator())));
        }

        const WideInteger unitsToDistribute = WideInteger{allocation.mTotal} - sumOfFlooredParts;

        if (unitsToDistribute < 0 || unitsToDistribute > aParts.size())
        {
            return NumericError::InvalidInput;
        }

        std::sort(partIndicesInPriorityOrder.begin(),
                  partIndicesInPriorityOrder.end(),
                  [&](std::size_t aLeft, std::size_t aRight) {
                      const auto comparison =
                          compareExact(fractionalRemainders[aLeft], fractionalRemainders[aRight]);

                      return comparison != 0 ? comparison > 0
                                             : aParts[aLeft].mKey < aParts[aRight].mKey;
                  });

        for (std::size_t index = 0; index < unitsToDistribute.convert_to<std::size_t>(); ++index)
        {
            const auto partIndex = partIndicesInPriorityOrder[index];
            const auto incrementedPartResult = checkedAdd(allocation.mParts[partIndex], 1);

            if (const auto* error = std::get_if<NumericError>(&incrementedPartResult))
            {
                return *error;
            }

            allocation.mParts[partIndex] = std::get<std::int64_t>(incrementedPartResult);
        }

        const auto emittedTotalInMajorUnits = ExactRational::create(allocation.mTotal, aScale);
        const auto exactResidualResult =
            subtractExact(exactTotal, std::get<ExactRational>(emittedTotalInMajorUnits));

        if (const auto* error = std::get_if<NumericError>(&exactResidualResult))
        {
            return *error;
        }

        allocation.mResidual = std::get<ExactRational>(exactResidualResult);

        return allocation;
    });
}

NumericResult<RoundedAllocation> allocateAdjustedUnits(std::span<const UnitAllocationPart> aLots,
                                                       std::int64_t aNewShares,
                                                       std::int64_t aOldShares) {
    if (aNewShares <= 0 || aOldShares <= 0 || aNewShares == aOldShares)
    {
        return NumericError::InvalidInput;
    }

    std::vector<ExactAllocationPart> exactAdjustedParts;

    for (const auto& lot : aLots)
    {
        if (lot.mUnits <= 0)
        {
            return NumericError::UnrepresentableValue;
        }

        const auto adjustedQuantityResult =
            ExactRational::create(WideInteger{lot.mUnits} * aNewShares,
                                  WideInteger{aOldShares} * UNITS_SCALE);

        if (const auto* error = std::get_if<NumericError>(&adjustedQuantityResult))
        {
            return *error;
        }

        exactAdjustedParts.push_back(
            {std::get<ExactRational>(adjustedQuantityResult), {lot.mLotOrder, 0}});
    }

    auto allocationResult = allocateLargestRemainder(exactAdjustedParts, UNITS_SCALE);

    if (const auto* allocation = std::get_if<RoundedAllocation>(&allocationResult))
    {
        if (std::any_of(allocation->mParts.begin(), allocation->mParts.end(), [](Units aUnits) {
                return aUnits == 0;
            }))
        {
            return NumericError::UnrepresentableValue;
        }
    }

    return allocationResult;
}

NumericResult<RoundedAllocation> allocateMoneyParts(std::span<const ExactAllocationPart> aParts) {
    return allocateLargestRemainder(aParts, MONEY_SCALE);
}

} // namespace taxbroker
