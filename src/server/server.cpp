#include "server.hpp"
#include "taxbroker/logging.hpp"

namespace taxbroker {

int StartServer() {
    LOG_TRACE("Trace message");
    LOG_DEBUG("Debug message");
    LOG_INFO("Info message");
    LOG_WARNING("Warning message");
    LOG_ERROR("Error message");
    LOG_CRITICAL("Critical message");

    return 0;
}

} // namespace taxbroker
