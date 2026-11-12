#include "large_allocation_hook.hpp"

#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/reference_catalogue.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <new>
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

void operator delete(void* aMemory) noexcept {
    std::free(aMemory);
}

void operator delete(void* aMemory, std::size_t) noexcept {
    std::free(aMemory);
}
