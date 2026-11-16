#include "server.hpp"

#include "http_server.hpp"
#include "launch_file.hpp"
#include "request_guard.hpp"
#include "routes.hpp"
#include "state_folder.hpp"
#include "static_files.hpp"

#include "taxbroker/logging.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <system_error>
#include <variant>

namespace taxbroker {

namespace {

constexpr std::chrono::seconds kRequestTimeout{30};
constexpr std::size_t kHeaderLimit = 8 * 1024;

/// Writes the error line and logs it. The text never holds a path or a value.
int fail(std::ostream& aErr, const std::string& aMessage) {
    LOG_ERROR("{}", aMessage);
    aErr << "Error: " << aMessage << '\n';

    return 1;
}

} // namespace

int StartServer(const EnvLookup& aEnv,
                std::ostream& aOut,
                std::ostream& aErr,
                const ServerHooks& aHooks) {
    const std::variant<ServerConfig, std::string> read = readServerConfig(aEnv);

    if (std::holds_alternative<std::string>(read))
    {
        return fail(aErr, std::get<std::string>(read));
    }

    const ServerConfig& config = std::get<ServerConfig>(read);

    if (!isLoopbackHost(config.mHost))
    {
        LOG_WARNING("The server listens on a non-loopback address; publish its port on "
                    "127.0.0.1 only.");
    }

    std::filesystem::path launchFile;
    // Declared in this order on purpose: members die in reverse, so the server (and the
    // connections still holding the handler lambdas that point at `routes`) goes first.
    std::unique_ptr<Routes> routes;
    std::unique_ptr<HttpServer> server;
    std::string linkToken;

    try
    {
        HttpServerOptions options;
        options.mTimeout = kRequestTimeout;
        options.mHeaderLimit = kHeaderLimit;
        options.mBodyLimit = kDefaultBodyLimit;
        options.mHandleSignals = aHooks.mHandleSignals;

        // Bind first: the port may be random, and the tokens and the link need the real one.
        server = std::make_unique<HttpServer>(config.mHost, config.mPort, options);

        // Two secrets (R4); they are kept in memory only.
        linkToken = makeToken();
        launchFile = launchFilePath(config, aEnv);
        routes = std::make_unique<Routes>(
            server->port(),
            linkToken,
            makeToken(),
            launchFile,
            []() { return std::chrono::steady_clock::now(); },
            aHooks.mMaxFileBytes,
            embeddedStaticFiles());

        // A TBR_LAUNCH_FILE folder is made by the caller (Docker: scripts/run.sh).
        if (!config.mLaunchFile.has_value())
        {
            makePrivateFolder(launchFile.parent_path());
        }

        writeLaunchFile(launchFile, makeLaunchPage(server->port(), linkToken));
    } catch (const std::exception& error)
    {
        // Every failure above throws a text without a path or a value.
        return fail(aErr, error.what());
    }

    const std::uint16_t port = server->port();

    // Docker: the host opens the file. Natively the opener only starts xdg-open, which may still
    // fail later (no desktop), so the link is printed in both cases.
    if (!config.mLaunchFile.has_value())
    {
        // The file name is not in the line: only whether the program started.
        if (aHooks.mOpener(launchFile))
        {
            LOG_INFO("The browser program was started");
        }
        else
        {
            LOG_WARNING("The browser program could not be started; the link in the output works");
        }
    }

    // The link is for the user, so it goes to the output, never to the log (R4, R13). Flushed at
    // once: a piped output is buffered, and the server writes nothing more until it stops.
    aOut << "Open http://127.0.0.1:" << port << "/#token=" << linkToken
         << " in your browser (it works once, for 2 minutes).\n"
         << std::flush;

    LOG_INFO("Server listening on {}:{}", config.mHost, port);

    if (aHooks.mOnListening)
    {
        aHooks.mOnListening(port, [&server]() { server->stop(); });
    }

    server->run([&routes](const HttpRequest& aRequest) { return routes->handle(aRequest); },
                [&routes](const HttpRequest& aHeader) { return routes->bodyPolicy(aHeader); });

    std::error_code ignored;
    std::filesystem::remove(launchFile, ignored);

    LOG_INFO("Server stopped");

    return 0;
}

} // namespace taxbroker
