#include "../support/property_random.hpp"
#include "taxbroker/fifo_basis.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace taxbroker;

constexpr std::uint64_t PROPERTY_SEED = 20261101;
constexpr int PROPERTY_ITERATIONS = 200;

template <typename T> T requireValue(const NumericResult<T>& aResult) {
    if (const auto* value = std::get_if<T>(&aResult))
    {
        return *value;
    }

    throw std::runtime_error{"Expected a successful synthetic arithmetic result"};
}

ExactRational ticks(std::int64_t aTicks) {
    return ExactRational{WideInteger{aTicks}};
}

/// The invariant of a partly consumed lot: allocated plus remaining basis is the original basis,
/// consumed plus remaining units are the original units, and an empty lot keeps no basis.
std::optional<std::string> basisConservationFailure(const LotBasisState& aOriginal,
                                                    const ExactRational& aAllocatedTicks,
                                                    const LotBasisState& aRemaining,
                                                    Units aConsumedUnits) {
    const auto sum = addExact(aAllocatedTicks, aRemaining.mRemainingBasis.mTicks);
    const auto* exactSum = std::get_if<ExactRational>(&sum);

    if (exactSum == nullptr || *exactSum != aOriginal.mRemainingBasis.mTicks)
    {
        return "allocated plus remaining basis differs from the original basis";
    }

    if (aConsumedUnits + aRemaining.mRemainingUnits != aOriginal.mRemainingUnits)
    {
        return "consumed plus remaining units differ from the original units";
    }

    if (aRemaining.mRemainingUnits == 0 && aRemaining.mRemainingBasis.mTicks != ExactRational{})
    {
        return "a lot without units keeps basis";
    }

    return std::nullopt;
}

TEST(FifoBasisPropertyTest, ThreeUnitsAtEur10SoldInTwoPartsKeepTheWholeBasis) {
    const LotBasisState original =
        requireValue(createLotBasis(1'000'000'000, 300'000'000)); // 3 units at EUR 10

    const auto first = requireValue(consumeLotBasis(original, 100'000'000));          // 1 unit
    const auto second = requireValue(consumeLotBasis(first.mRemaining, 200'000'000)); // 2 units

    EXPECT_EQ(original.mRemainingBasis.mTicks, ticks(300'000'000'000'000'000)); // EUR 30
    EXPECT_EQ(first.mAllocatedBasis.mTicks, ticks(100'000'000'000'000'000));    // EUR 10
    EXPECT_EQ(second.mAllocatedBasis.mTicks, ticks(200'000'000'000'000'000));   // EUR 20
    EXPECT_EQ(second.mRemaining, LotBasisState{});

    EXPECT_EQ(basisConservationFailure(original,
                                       first.mAllocatedBasis.mTicks,
                                       first.mRemaining,
                                       100'000'000),
              std::nullopt);
    EXPECT_EQ(basisConservationFailure(original,
                                       ticks(300'000'000'000'000'000),
                                       second.mRemaining,
                                       300'000'000),
              std::nullopt);
}

TEST(FifoBasisPropertyTest, AllocatedPlusRemainingBasisEqualsTheOriginalBasisAfterEveryStep) {
    for (int iteration = 0; iteration < PROPERTY_ITERATIONS; ++iteration)
    {
        const auto seed = PROPERTY_SEED + static_cast<std::uint64_t>(iteration);
        SCOPED_TRACE("seed " + std::to_string(seed));
        test::PropertyRandom random{seed};

        // A unit price up to 10'000 EUR and up to 10'000 units, both at eight decimals.
        const UnitPrice price = random.between(1, 1'000'000'000'000);
        const Units units = random.between(1, 1'000'000'000'000);
        const LotBasisState original = requireValue(createLotBasis(price, units));

        LotBasisState remaining = original;
        ExactRational allocatedTicks;
        Units consumedUnits = 0;

        while (remaining.mRemainingUnits > 0)
        {
            Units matched = random.between(1, remaining.mRemainingUnits);

            if (random.below(4) == 0)
            {
                matched = remaining.mRemainingUnits;
            }

            const auto step = requireValue(consumeLotBasis(remaining, matched));

            allocatedTicks = requireValue(addExact(allocatedTicks, step.mAllocatedBasis.mTicks));
            consumedUnits += matched;
            remaining = step.mRemaining;

            EXPECT_EQ(basisConservationFailure(original, allocatedTicks, remaining, consumedUnits),
                      std::nullopt);
        }

        EXPECT_EQ(allocatedTicks, original.mRemainingBasis.mTicks);
        EXPECT_EQ(remaining, LotBasisState{});
    }
}

TEST(FifoBasisPropertyTest, TheCheckReportsABasisThatIsOneTickShort) {
    const LotBasisState original = requireValue(createLotBasis(1'000'000'000, 300'000'000));
    const LotBasisState remaining{
        .mRemainingUnits = 200'000'000,
        .mRemainingBasis = {ticks(200'000'000'000'000'000)},
    };

    // EUR 10 allocated would be 100'000'000'000'000'000 ticks; one tick less loses a tick.
    EXPECT_EQ(
        basisConservationFailure(original, ticks(99'999'999'999'999'999), remaining, 100'000'000),
        "allocated plus remaining basis differs from the original basis");
}

TEST(FifoBasisPropertyTest, TheCheckReportsUnitsThatDoNotAddUp) {
    const LotBasisState original = requireValue(createLotBasis(1'000'000'000, 300'000'000));
    const LotBasisState remaining{
        .mRemainingUnits = 200'000'000,
        .mRemainingBasis = {ticks(200'000'000'000'000'000)},
    };

    // 1 unit consumed and 2 remaining add up to 3, but the test says 2 units were consumed.
    EXPECT_EQ(
        basisConservationFailure(original, ticks(100'000'000'000'000'000), remaining, 200'000'000),
        "consumed plus remaining units differ from the original units");
}

TEST(FifoBasisPropertyTest, TheCheckReportsAnEmptyLotThatKeepsBasis) {
    const LotBasisState original = requireValue(createLotBasis(1'000'000'000, 300'000'000));
    const LotBasisState remaining{
        .mRemainingUnits = 0,
        .mRemainingBasis = {ticks(100'000'000'000'000'000)},
    };

    // All 3 units consumed for EUR 20, and EUR 10 stay on a lot with no units.
    EXPECT_EQ(
        basisConservationFailure(original, ticks(200'000'000'000'000'000), remaining, 300'000'000),
        "a lot without units keeps basis");
}

} // namespace
