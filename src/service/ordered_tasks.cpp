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

    const std::size_t limit = aLimit == 0 ? kDefaultParseWorkerLimit : aLimit;

    return std::min({aTaskCount, limit, std::max<std::size_t>(1, aHardwareThreads)});
}

} // namespace taxbroker
