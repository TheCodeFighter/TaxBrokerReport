#include "service/ordered_tasks.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace taxbroker {

std::size_t parseWorkerCount(const std::size_t aTaskCount,
                             const std::size_t aLimit,
                             const std::size_t aHardwareThreads) {
    if (aTaskCount == 0)
    {
        return 0;
    }

    return std::min(
        {aTaskCount, std::max<std::size_t>(1, aLimit), std::max<std::size_t>(1, aHardwareThreads)});
}

void checkStartOrder(const std::size_t aTaskCount, const std::vector<std::size_t>& aStartOrder) {
    if (aStartOrder.empty())
    {
        return;
    }

    if (aStartOrder.size() != aTaskCount)
    {
        throw std::invalid_argument("A start order must name every task once.");
    }

    std::vector<bool> seen(aTaskCount, false);

    for (const std::size_t index : aStartOrder)
    {
        if (index >= aTaskCount || seen[index])
        {
            throw std::invalid_argument("A start order must name every task once.");
        }

        seen[index] = true;
    }
}

} // namespace taxbroker
