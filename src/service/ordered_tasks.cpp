#include "service/ordered_tasks.hpp"

#include <algorithm>

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

} // namespace taxbroker
