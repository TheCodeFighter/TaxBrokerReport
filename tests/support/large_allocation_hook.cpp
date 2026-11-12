#include "large_allocation_hook.hpp"

#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/reference_catalogue.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace taxbroker::test {
namespace {

// Constant-initialised, so `operator new` may run before the other globals exist.
std::atomic<bool> gArmed{false};
std::function<void()>* gAction = nullptr;

// The action runs without taking part in the hook itself.
thread_local bool tInsideAction = false;

} // namespace

bool allocationHookRuns() {
    // Valgrind replaces `operator new` of the test binary unless it is started with
    // `--soname-synonyms=somalloc=nouserintercepts`; then the hook never runs. A real probe
    // tells that apart from a hook that works, whatever the reason.
    static const bool runs = [] {
        bool ran = false;

        {
            const ScopedLargeAllocationHook hook{[&ran] { ran = true; }};

            // A volatile pointer keeps the compiler from dropping the allocation.
            void* volatile memory = ::operator new(kLargeAllocationBytes);

            ::operator delete(memory);
        }

        return ran;
    }();

    return runs;
}

bool allocationHookIsRequired() {
    const char* required = std::getenv("TBR_REQUIRE_ALLOCATION_HOOK");

    return required != nullptr && std::string_view{required} == "1";
}

ScopedLargeAllocationHook::ScopedLargeAllocationHook(std::function<void()> aAction) {
    gAction = new std::function<void()>{std::move(aAction)};
    gArmed.store(true);
}

ScopedLargeAllocationHook::~ScopedLargeAllocationHook() {
    gArmed.store(false);
    delete std::exchange(gAction, nullptr);
}

ScopedAllocationFailure::ScopedAllocationFailure(const std::size_t aFailFromAllocation) {
    (void)makeDiagnosticMessage(MessageKey::InputFileOutOfMemory);
    (void)loadReferenceData(std::vector<std::filesystem::path>{});

    mHook.emplace([this, aFailFromAllocation] {
        if (++mCount >= aFailFromAllocation)
        {
            throw std::bad_alloc{};
        }
    });
}

} // namespace taxbroker::test

void* operator new(const std::size_t aBytes) {
    using namespace taxbroker::test;

    if (aBytes >= kLargeAllocationBytes && gArmed.load() && !tInsideAction)
    {
        tInsideAction = true;

        try
        { (*gAction)(); } catch (...)
        {
            tInsideAction = false;

            throw;
        }

        tInsideAction = false;
    }

    if (void* memory = std::malloc(aBytes != 0 ? aBytes : 1))
    {
        return memory;
    }

    throw std::bad_alloc{};
}

// The library allocates with this form too (the buffer of std::stable_sort, the thread-local
// destructors) and frees the memory with the `operator delete` below. When it stays the library's
// own, Valgrind sees a `new` freed with `free` and reports a mismatch.
void* operator new(const std::size_t aBytes, const std::nothrow_t&) noexcept {
    try
    { return operator new(aBytes); } catch (...)
    { return nullptr; }
}

void operator delete(void* aMemory) noexcept {
    std::free(aMemory);
}

void operator delete(void* aMemory, std::size_t) noexcept {
    std::free(aMemory);
}
