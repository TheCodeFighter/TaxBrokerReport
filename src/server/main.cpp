#include "browser_open.hpp"
#include "server.hpp"
#include "server_config.hpp"

#include "taxbroker/build_info.hpp"
#include "taxbroker/input_file.hpp"
#include "taxbroker/logging.hpp"

#include <spdlog/spdlog.h>

#include <iostream>
#include <string>
#include <vector>

int main(int aArgc, char** aArgv) {
    const std::vector<std::string> arguments(aArgv + 1, aArgv + aArgc);

    // Before the logger: the version starts nothing (no log file, no server, no launch file).
    if (taxbroker::isVersionRequest(arguments))
    {
        std::cout << taxbroker::buildInfoText() << '\n';

        return 0;
    }

    // Called just one time!
    taxbroker::InitializeLogger();
    taxbroker::logProgramStart("server");

    taxbroker::ServerHooks hooks;
    hooks.mOpener = taxbroker::openInBrowser;
    hooks.mHandleSignals = true;
    hooks.mMaxFileBytes = taxbroker::kMaxInputFileBytes;

    hooks.mNoLoginLimit = taxbroker::kNoLoginLimit;
    hooks.mIdleLimit = taxbroker::kIdleLimit;
    hooks.mClosedPageLimit = taxbroker::kClosedPageLimit;

    const int exitCode =
        taxbroker::StartServer(taxbroker::systemEnvironment(), std::cout, std::cerr, hooks);

    // Called just one time!
    spdlog::shutdown();

    return exitCode;
}
