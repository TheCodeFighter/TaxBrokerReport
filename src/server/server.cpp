#include "utils/logger.hpp"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    taxbroker::InitializeLogger();

    LOG_TRACE("Trace message");
    LOG_DEBUG("Debug message");
    LOG_INFO("Info message");
    LOG_WARNING("Warning message");
    LOG_ERROR("Error message");
    LOG_CRITICAL("Critical message");

    return 0;
}
