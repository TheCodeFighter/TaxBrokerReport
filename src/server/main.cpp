#include "browser_open.hpp"
#include "server.hpp"
#include "server_config.hpp"

#include "taxbroker/logging.hpp"

#include <spdlog/spdlog.h>

#include <iostream>

int main(int aArgc, char** aArgv) {
    (void)aArgc;
    (void)aArgv;

    // Called just one time!
    taxbroker::InitializeLogger();

    taxbroker::ServerHooks hooks;
    hooks.mOpener = taxbroker::openInBrowser;
    hooks.mHandleSignals = true;

    const int exitCode =
        taxbroker::StartServer(taxbroker::systemEnvironment(), std::cout, std::cerr, hooks);

    // Called just one time!
    spdlog::shutdown();

    return exitCode;
}
