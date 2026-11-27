#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace taxbroker {

/// The default most workers that parse files at the same time.
inline constexpr std::size_t kDefaultParseWorkerLimit = 4;

/// How many workers run `aTaskCount` tasks: the smallest of the task count, the limit (0 = the
/// default limit) and the hardware threads (0 = unknown, then 1). 0 for no task.
[[nodiscard]] std::size_t
parseWorkerCount(std::size_t aTaskCount, std::size_t aLimit, std::size_t aHardwareThreads);

/// The result of one task: its value, or the exception it threw.
template <typename Value> struct TaskOutcome {
    std::optional<Value> mValue;   ///< Set when the task returned.
    std::exception_ptr mException; ///< Set when the task threw.
};

/// Runs `aTask(index)` for every index below `aTaskCount` and returns the outcomes in index order,
/// whatever order the tasks finish in. With one worker (or none) every task runs on the calling
/// thread in index order and no thread starts; otherwise the calling thread and up to
/// `aWorkerCount - 1` threads take the next index and write only their own slot. A thread that
/// cannot start is not an error: the others do the work. Every thread is joined before it returns.
template <typename Task>
[[nodiscard]] auto
runTasksInOrder(const std::size_t aTaskCount, const std::size_t aWorkerCount, const Task& aTask) {
    using Value = decltype(aTask(std::size_t{}));

    std::vector<TaskOutcome<Value>> outcomes(aTaskCount);
    std::atomic<std::size_t> nextIndex{0};

    const auto work = [&]() {
        for (std::size_t index = nextIndex.fetch_add(1); index < aTaskCount;
             index = nextIndex.fetch_add(1))
        {
            try
            { outcomes[index].mValue.emplace(aTask(index)); } catch (...)
            { outcomes[index].mException = std::current_exception(); }
        }
    };

    std::vector<std::thread> threads;

    if (aWorkerCount > 1)
    {
        threads.reserve(aWorkerCount - 1);

        for (std::size_t worker = 1; worker < aWorkerCount; ++worker)
        {
            // `std::system_error` when no thread is left, `std::bad_alloc` for the thread state.
            try
            { threads.emplace_back(work); } catch (const std::exception&)
            { break; }
        }
    }

    work();

    for (std::thread& thread : threads)
    {
        thread.join();
    }

    return outcomes;
}

} // namespace taxbroker
