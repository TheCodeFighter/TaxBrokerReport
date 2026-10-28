#include "taxbroker/split_ratio.hpp"
#include "taxbroker/exact_arithmetic.hpp"

#include <numeric>
#include <variant>

namespace taxbroker {

std::optional<SplitRatio> makeSplitRatio(std::int64_t aNewShares, std::int64_t aOldShares) {
    if (aNewShares <= 0 || aOldShares <= 0 || aNewShares == aOldShares)
    {
        return std::nullopt;
    }

    const auto commonDivisor = std::gcd(aNewShares, aOldShares);

    return SplitRatio{
        .mNewShares = aNewShares / commonDivisor,
        .mOldShares = aOldShares / commonDivisor,
    };
}

std::optional<SplitRatio> splitRatioFromShareCounts(Units aSharesBefore, Units aSharesAfter) {
    // Both counts have the same scale, so the scale cancels out: 0.52 and 5.20 are
    // 52000000 and 520000000, which reduce to 10 / 1.
    return makeSplitRatio(aSharesAfter, aSharesBefore);
}

std::optional<Units> previewSplitUnits(Units aPositionBefore, const SplitRatio& aRatio) {
    // The position is in ticks of 10^-8 units; the exact value in whole units is
    // ticks * new / (old * 10^8).
    const auto exact = ExactRational::create(WideInteger{aPositionBefore} * aRatio.mNewShares,
                                             WideInteger{aRatio.mOldShares} * UNITS_SCALE);

    if (!std::holds_alternative<ExactRational>(exact))
    {
        return std::nullopt;
    }

    const auto rounded = roundToScaled(std::get<ExactRational>(exact),
                                       UNITS_SCALE,
                                       DestinationRange{.mPolicy = ValuePolicy::Positive});

    if (!std::holds_alternative<std::int64_t>(rounded))
    {
        return std::nullopt;
    }

    return std::get<std::int64_t>(rounded);
}

bool isUnusualSplitRatio(const SplitRatio& aRatio) {
    return aRatio.mNewShares > kUnusualSplitRatioLimit ||
           aRatio.mOldShares > kUnusualSplitRatioLimit;
}

} // namespace taxbroker
