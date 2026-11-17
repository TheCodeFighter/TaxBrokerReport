#include "browser_open.hpp"
#include "server.hpp"
#include "server_config.hpp"

#include "taxbroker/build_info.hpp"
#include "taxbroker/input_file.hpp"
#include "taxbroker/logging.hpp"

#include <spdlog/spdlog.h>

#include <iostream>

int main(int aArgc, char** aArgv) {
    (void)aArgc;
    (void)aArgv;

    // Called just one time!
    taxbroker::InitializeLogger();
    taxbroker::logProgramStart("server");

    taxbroker::ServerHooks hooks;
    hooks.mOpener = taxbroker::openInBrowser;
    hooks.mHandleSignals = true;
    hooks.mMaxFileBytes = taxbroker::kMaxInputFileBytes;

    const int exitCode =
        taxbroker::StartServer(taxbroker::systemEnvironment(), std::cout, std::cerr, hooks);

    // Called just one time!
    spdlog::shutdown();

    return exitCode;
}
