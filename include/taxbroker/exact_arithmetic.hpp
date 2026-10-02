#pragma once

#include "taxbroker/types.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <variant>
#include <vector>

namespace taxbroker {

using WideInteger = boost::multiprecision::checked_int256_t;

enum class NumericError {
    InvalidInput,
    InvalidDenominator,
    InvalidExchangeRate,
    Overflow,
    UnrepresentableValue,
};

template <typename T> using NumericResult = std::variant<T, NumericError>;

enum class ValuePolicy {
    Signed,
    Nonnegative,
    Positive,
};

struct DestinationRange {
    std::int64_t mMinimum{std::numeric_limits<std::int64_t>::min()};
    std::int64_t mMaximum{std::numeric_limits<std::int64_t>::max()};
    ValuePolicy mPolicy{ValuePolicy::Signed};
};

/// Stores a number as a reduced numerator/denominator fraction, e.g. 1/3, with a positive
/// denominator. Preserves fractional basis and allocation results until explicit boundary rounding.
class ExactRational {
  public:
    /// Creates exact zero (0/1).
    ExactRational() = default;
    /// Stores an integer exactly with denominator 1.
    explicit ExactRational(WideInteger aInteger);

    /// Normalizes the denominator's sign and reduces the fraction; rejects a zero denominator.
    [[nodiscard]] static NumericResult<ExactRational> create(WideInteger aNumerator,
                                                             WideInteger aDenominator);
    /// Returns the signed numerator of the reduced fraction.
    [[nodiscard]] const WideInteger& numerator() const noexcept;
    /// Returns the positive denominator of the reduced fraction.
    [[nodiscard]] const WideInteger& denominator() const noexcept;

    bool operator==(const ExactRational&) const = default;

  private:
    ExactRational(WideInteger aNumerator, WideInteger aDenominator);

    WideInteger mNumerator{};
    WideInteger mDenominator{1};
};

[[nodiscard]] NumericResult<std::int64_t> checkedAdd(std::int64_t aLeft, std::int64_t aRight);
[[nodiscard]] NumericResult<std::int64_t> checkedSubtract(std::int64_t aLeft, std::int64_t aRight);
/// Returns the absolute value; INT64_MIN fails because its positive magnitude cannot fit.
[[nodiscard]] NumericResult<std::int64_t> checkedMagnitude(std::int64_t aValue);
/// Checks the destination's bounds and sign policy, including rejecting zero for Positive.
[[nodiscard]] NumericResult<std::int64_t> validateDestination(std::int64_t aValue,
                                                              DestinationRange aRange = {});

/// Adds exact fractions, reducing common factors before products; checked overflow returns
/// Overflow.
[[nodiscard]] NumericResult<ExactRational> addExact(const ExactRational& aLeft,
                                                    const ExactRational& aRight);
/// Subtracts exact fractions, reducing common factors before products; checked overflow returns
/// Overflow.
[[nodiscard]] NumericResult<ExactRational> subtractExact(const ExactRational& aLeft,
                                                         const ExactRational& aRight);
/// Multiplies exact fractions after cancelling numerator/denominator factors to limit growth.
[[nodiscard]] NumericResult<ExactRational> multiplyExact(const ExactRational& aLeft,
                                                         const ExactRational& aRight);
/// Multiplies by the exact reciprocal; a zero divisor returns InvalidDenominator.
[[nodiscard]] NumericResult<ExactRational> divideExact(const ExactRational& aLeft,
                                                       const ExactRational& aRight);
/// Returns -1, 0 or 1 without cross-multiplying potentially large numerators and denominators.
[[nodiscard]] int compareExact(const ExactRational& aLeft, const ExactRational& aRight);

/// Round directly from an exact value in major units to the destination's scaled integer.
/// Half values round away from zero; the result must satisfy the destination's bounds and sign
/// policy.
[[nodiscard]] NumericResult<std::int64_t>
roundToScaled(const ExactRational& aValue, std::int64_t aScale, DestinationRange aRange = {});

enum class ExchangeRateDirection {
    ForeignPerEur,
    EurPerForeign,
};

/// Converts foreign Money to EUR using a rate stored at EXCHANGE_RATE_SCALE.
/// ForeignPerEur divides by the rate; EurPerForeign multiplies. Rounds once to MONEY_SCALE.
[[nodiscard]] NumericResult<Money> convertMoney(Money aForeignMoney,
                                                ExchangeRate aRate,
                                                ExchangeRateDirection aDirection,
                                                DestinationRange aRange = {});

struct AllocationKey {
    std::size_t mLotOrder{};
    std::size_t mSaleOrder{};

    auto operator<=>(const AllocationKey&) const = default;
};

struct ExactAllocationPart {
    ExactRational mValue;
    AllocationKey mKey;
};

struct RoundedAllocation {
    std::vector<std::int64_t> mParts;
    std::int64_t mTotal{};
    /// Exact combined value minus emitted combined value, in major units.
    ExactRational mResidual;
};

/// Rounds the combined nonnegative total once, floors each scaled part, then distributes extra
/// units by largest fractional remainder. Ties use lot then sale order; returned parts retain input
/// order.
[[nodiscard]] NumericResult<RoundedAllocation>
allocateLargestRemainder(std::span<const ExactAllocationPart> aParts, std::int64_t aScale);

struct UnitAllocationPart {
    Units mUnits{};
    std::size_t mLotOrder{};
};

/// Applies the exact new/old share ratio, then allocates the pooled total at UNITS_SCALE.
/// Ties favor earlier lots; rejects an allocation that reduces a nonempty lot to zero units.
/// Action identity and impact-budget validation belong to callers.
[[nodiscard]] NumericResult<RoundedAllocation> allocateAdjustedUnits(
    std::span<const UnitAllocationPart> aLots, std::int64_t aNewShares, std::int64_t aOldShares);
/// Allocates exact EUR parts at MONEY_SCALE so their emitted sum equals the once-rounded total.
[[nodiscard]] NumericResult<RoundedAllocation>
allocateMoneyParts(std::span<const ExactAllocationPart> aParts);

} // namespace taxbroker
