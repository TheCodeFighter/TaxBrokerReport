#include "utils/logger.hpp"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    taxbroker::InitializeLogger();

    spdlog::debug("Debug message");
    spdlog::info("Info message");
    spdlog::warn("Warning message");
    spdlog::error("Error message");

    return 0;
}
