#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace taxbroker {

/// How many workers run `aTaskCount` tasks: the smallest of the task count, the limit (0 counts as
/// 1) and the hardware threads (0 = unknown, then 1). 0 for no task.
[[nodiscard]] std::size_t
parseWorkerCount(std::size_t aTaskCount, std::size_t aLimit, std::size_t aHardwareThreads);

/// The result of one task: its value, or the exception it threw.
template <typename Value> struct TaskOutcome {
    std::optional<Value> mValue;   ///< Set when the task returned.
    std::exception_ptr mException; ///< Set when the task threw.
};

/// Throws `std::invalid_argument` unless `aStartOrder` is empty or holds every index below
/// `aTaskCount` exactly once.
void checkStartOrder(std::size_t aTaskCount, const std::vector<std::size_t>& aStartOrder);

/// Runs `aTask(index)` for every index below `aTaskCount` and returns the outcomes in index order,
/// whatever order the tasks finish in. The tasks are taken in `aStartOrder` (empty = index order;
/// tests use it to change the finish order). With one worker (or none) every task runs on the
/// calling thread and no thread starts; otherwise the calling thread and up to `aWorkerCount - 1`
/// threads take the next task and write only its slot. A thread that cannot start is not an error.
template <typename Task>
[[nodiscard]] auto runTasksInOrder(const std::size_t aTaskCount,
                                   const std::size_t aWorkerCount,
                                   const Task& aTask,
                                   const std::vector<std::size_t>& aStartOrder) {
    using Value = decltype(aTask(std::size_t{}));

    checkStartOrder(aTaskCount, aStartOrder);

    std::vector<TaskOutcome<Value>> outcomes(aTaskCount);
    std::atomic<std::size_t> nextPosition{0};

    const auto work = [&]() {
        for (std::size_t position = nextPosition.fetch_add(1); position < aTaskCount;
             position = nextPosition.fetch_add(1))
        {
            const std::size_t index = aStartOrder.empty() ? position : aStartOrder[position];

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

/// As above, with the tasks taken in index order.
template <typename Task>
[[nodiscard]] auto
runTasksInOrder(const std::size_t aTaskCount, const std::size_t aWorkerCount, const Task& aTask) {
    return runTasksInOrder(aTaskCount, aWorkerCount, aTask, std::vector<std::size_t>{});
}

} // namespace taxbroker
