#pragma once

#include <cstdint>

namespace taxbroker::test {

/// A small random number generator (SplitMix64) for property tests. Its numbers depend only on the
/// seed, on every compiler and library, so a failing case is replayed by its seed alone.
class PropertyRandom {
  public:
    explicit PropertyRandom(std::uint64_t aSeed) : mState(aSeed) {}

    std::uint64_t next() {
        mState += 0x9E3779B97F4A7C15ULL;

        std::uint64_t value = mState;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;

        return value ^ (value >> 31);
    }

    /// A number from 0 to aCount - 1. The small bias of the modulo does not matter in tests.
    std::uint64_t below(std::uint64_t aCount) {
        return next() % aCount;
    }

    /// A number from aLow to aHigh, both included.
    std::int64_t between(std::int64_t aLow, std::int64_t aHigh) {
        return aLow +
               static_cast<std::int64_t>(below(static_cast<std::uint64_t>(aHigh - aLow) + 1));
    }

  private:
    std::uint64_t mState;
};

} // namespace taxbroker::test
