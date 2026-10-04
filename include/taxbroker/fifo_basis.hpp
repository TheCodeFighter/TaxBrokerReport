#pragma once

#include "taxbroker/exact_arithmetic.hpp"

namespace taxbroker {

/// Exact rational ticks of 10^-12 EUR, including residuals after partial consumption.
struct ExactBasis {
    ExactRational mTicks;

    bool operator==(const ExactBasis&) const = default;
};

/// One lot's remaining quantity and exact basis; dates, sources and FIFO order belong to its
/// caller. A fully consumed lot has zero units and zero basis.
struct LotBasisState {
    Units mRemainingUnits{};
    ExactBasis mRemainingBasis;

    bool operator==(const LotBasisState&) const = default;
};

/// Exact basis assigned to a disposal together with the next, independently returned lot state.
struct BasisConsumption {
    ExactBasis mAllocatedBasis;
    LotBasisState mRemaining;
};

/// Creates exact basis from a positive four-decimal EUR unit price and eight-decimal quantity.
/// The caller supplies the price already converted at the purchase-date rate.
[[nodiscard]] NumericResult<LotBasisState> createLotBasis(Money aConvertedUnitPrice,
                                                          Units aOriginalUnits);
/// Allocates basis proportionally without rounding; consuming all units takes all remaining basis.
/// Requires an open positive lot and 0 < matched units <= remaining units. Never mutates aLot.
[[nodiscard]] NumericResult<BasisConsumption> consumeLotBasis(const LotBasisState& aLot,
                                                              Units aMatchedUnits);
/// Replaces an open lot's quantity with a positive quantity while preserving its exact basis.
/// The caller resolves the action ratio and quantity allocation; this helper never mutates aLot.
[[nodiscard]] NumericResult<LotBasisState> adjustLotUnits(const LotBasisState& aLot,
                                                          Units aAdjustedUnits);
/// Converts basis ticks to exact EUR without rounding to Money or XML precision.
[[nodiscard]] NumericResult<ExactRational> basisInEur(const ExactBasis& aBasis);
/// Returns exact EUR per whole unit for an open positive lot, including quantity adjustments.
[[nodiscard]] NumericResult<ExactRational> exactLotUnitValue(const LotBasisState& aLot);

} // namespace taxbroker
