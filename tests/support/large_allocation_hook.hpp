#pragma once

#include <cstddef>
#include <functional>
#include <optional>

namespace taxbroker::test {

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
