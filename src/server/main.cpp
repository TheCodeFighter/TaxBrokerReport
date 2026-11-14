#include "server.hpp"
#include "taxbroker/logging.hpp"

#include <spdlog/spdlog.h>

int main(int aArgc, char** aArgv) {
    (void)aArgc;
    (void)aArgv;

    // Called just one time!
    taxbroker::InitializeLogger();

    const int exitCode = taxbroker::StartServer();

    // Called just one time!
    spdlog::shutdown();

    return exitCode;
}
