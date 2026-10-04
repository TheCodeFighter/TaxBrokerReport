#include "taxbroker/fifo_basis.hpp"

namespace taxbroker {
namespace {

bool validOpenLot(const LotBasisState& aLot) {
    return aLot.mRemainingUnits > 0 && aLot.mRemainingBasis.mTicks.numerator() > 0;
}

} // namespace

NumericResult<LotBasisState> createLotBasis(Money aConvertedUnitPrice, Units aOriginalUnits) {
    if (aConvertedUnitPrice <= 0 || aOriginalUnits <= 0)
    {
        return NumericError::UnrepresentableValue;
    }

    // Money has four decimals and Units eight: their raw product is exact 10^-12 EUR ticks.
    // Widen before multiplication so the product is not limited to the int64 input range.
    return LotBasisState{
        aOriginalUnits,
        ExactBasis{ExactRational{WideInteger{aConvertedUnitPrice} * aOriginalUnits}},
    };
}

NumericResult<BasisConsumption> consumeLotBasis(const LotBasisState& aLot, Units aMatchedUnits) {
    if (!validOpenLot(aLot) || aMatchedUnits <= 0 || aMatchedUnits > aLot.mRemainingUnits)
    {
        return NumericError::UnrepresentableValue;
    }

    if (aMatchedUnits == aLot.mRemainingUnits)
    {
        // Transfer the complete residual, including fractional ticks, and close the lot exactly.
        return BasisConsumption{aLot.mRemainingBasis, LotBasisState{}};
    }

    // Validation guarantees a positive denominator and int64 operands, so creation cannot fail.
    // Allocate from the current basis/quantity, retaining fractions through repeated disposals.
    const auto fraction = ExactRational::create(aMatchedUnits, aLot.mRemainingUnits);
    const auto allocated =
        multiplyExact(aLot.mRemainingBasis.mTicks, std::get<ExactRational>(fraction));

    if (const auto* error = std::get_if<NumericError>(&allocated))
    {
        return *error;
    }

    // Subtract the exact allocation: allocated + remaining must equal the previous basis.
    const auto remaining =
        subtractExact(aLot.mRemainingBasis.mTicks, std::get<ExactRational>(allocated));

    if (const auto* error = std::get_if<NumericError>(&remaining))
    {
        return *error;
    }

    return BasisConsumption{
        ExactBasis{std::get<ExactRational>(allocated)},
        LotBasisState{aLot.mRemainingUnits - aMatchedUnits,
                      ExactBasis{std::get<ExactRational>(remaining)}},
    };
}

NumericResult<LotBasisState> adjustLotUnits(const LotBasisState& aLot, Units aAdjustedUnits) {
    if (!validOpenLot(aLot) || aAdjustedUnits <= 0)
    {
        return NumericError::UnrepresentableValue;
    }

    // A units-only adjustment changes the value per unit, never the total purchase cost.
    return LotBasisState{aAdjustedUnits, aLot.mRemainingBasis};
}

NumericResult<ExactRational> basisInEur(const ExactBasis& aBasis) {
    constexpr std::int64_t basisScale = MONEY_SCALE * UNITS_SCALE;

    return divideExact(aBasis.mTicks, ExactRational{basisScale});
}

NumericResult<ExactRational> exactLotUnitValue(const LotBasisState& aLot) {
    if (!validOpenLot(aLot))
    {
        return NumericError::UnrepresentableValue;
    }

    // (basis ticks / (Money scale * Units scale)) / (raw units / Units scale):
    // the Units scale cancels, leaving exact EUR per whole unit.
    return divideExact(aLot.mRemainingBasis.mTicks,
                       ExactRational{WideInteger{MONEY_SCALE} * aLot.mRemainingUnits});
}

} // namespace taxbroker
