#include "cli/report_command.hpp"
#include "utils/logger.hpp"

#include <spdlog/spdlog.h>

#include <iostream>
#include <string>
#include <vector>

int main(int aArgc, char** aArgv) {
    const std::vector<std::string> arguments(aArgv + 1, aArgv + aArgc);

    // Called just one time!
    taxbroker::InitializeLogger();

    const int exitCode = taxbroker::cli::runCli(arguments, std::cin, std::cout, std::cerr);

    // Called just one time!
    spdlog::shutdown();

    return exitCode;
}
