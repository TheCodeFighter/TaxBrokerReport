#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <optional>

namespace taxbroker::test {

/// True when the allocation hook below works in this process: a large allocation runs the action.
/// It does not under Valgrind, which replaces `operator new` of the test binary, unless Valgrind
/// is started with `--soname-synonyms=somalloc=nouserintercepts` (see `scripts/valgrind.sh`).
bool allocationHookRuns();

/// True when the environment variable `TBR_REQUIRE_ALLOCATION_HOOK` is `1`: a test that needs the
/// hook then fails instead of skipping when the hook does not run.
bool allocationHookIsRequired();

/// An allocation of at least this many bytes counts as a large one. Test inputs that must reach
/// a large allocation are at least this big; every other allocation of a test is smaller.
inline constexpr std::size_t kLargeAllocationBytes = 64U * 1024U;

/// While it lives, runs aAction at every large allocation, before the memory is taken. The action
/// may throw std::bad_alloc; the allocation then fails. It must not take a large allocation
/// itself. The test binary replaces the global `operator new` (`large_allocation_hook.cpp`), so
/// the production code has no hook for this.
class ScopedLargeAllocationHook {
  public:
    explicit ScopedLargeAllocationHook(std::function<void()> aAction);

    ScopedLargeAllocationHook(const ScopedLargeAllocationHook&) = delete;
    ScopedLargeAllocationHook& operator=(const ScopedLargeAllocationHook&) = delete;

    ~ScopedLargeAllocationHook();
};

/// While it lives, the large allocations fail with std::bad_alloc from the n-th one (the first by
/// default), counted from its creation. It first builds the tables the app builds on first use
/// (the message table and the built-in reference lists), so only the code under test meets the
/// failure, whichever test runs first in its process.
class ScopedAllocationFailure {
  public:
    explicit ScopedAllocationFailure(std::size_t aFailFromAllocation = 1);

  private:
    std::size_t mCount{0};
    std::optional<ScopedLargeAllocationHook> mHook;
};

} // namespace taxbroker::test

/// Starts a test that needs the allocation hook. When the hook does not run (under Valgrind
/// without the flag above) the test is skipped, or fails when `TBR_REQUIRE_ALLOCATION_HOOK=1`, so
/// the Valgrind run that sets it cannot skip these tests without anyone noticing. A test with the
/// hook has `RunningOutOfMemory` or `AllocationHook` in its name: `scripts/valgrind.sh` selects
/// them by that.
#define SKIP_IF_ALLOCATION_HOOK_IS_UNAVAILABLE()                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!taxbroker::test::allocationHookRuns())                                                \
        {                                                                                          \
            if (taxbroker::test::allocationHookIsRequired())                                       \
            {                                                                                      \
                FAIL() << "The allocation hook does not run, but it is required.";                 \
            }                                                                                      \
                                                                                                   \
            GTEST_SKIP() << "Valgrind replaces operator new, so the hook cannot run.";             \
        }                                                                                          \
    } while (false)
