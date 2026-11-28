#include "server.hpp"

#include "http_server.hpp"
#include "launch_file.hpp"
#include "profile_store.hpp"
#include "request_guard.hpp"
#include "routes.hpp"
#include "state_folder.hpp"
#include "start_failure_page.hpp"
#include "static_files.hpp"

#include "taxbroker/build_info.hpp"
#include "taxbroker/logging.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
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

/// `fail`, and on a native start (no `TBR_LAUNCH_FILE`; Docker shows the error in its terminal)
/// the start-failure page in the browser, as the user may have started the program without a
/// terminal.
int failStart(const EnvLookup& aEnv,
              const ServerHooks& aHooks,
              std::ostream& aErr,
              const std::string& aMessage) {
    const int exitCode = fail(aErr, aMessage);

    if (aEnv("TBR_LAUNCH_FILE").has_value())
    {
        return exitCode;
    }

    const std::optional<std::filesystem::path> page =
        writeStartFailurePage(aEnv, aMessage, buildVersion());

    if (!page.has_value())
    {
        return exitCode;
    }

    // The file name is not in the line: only whether the program started.
    if (aHooks.mOpener(*page))
    {
        LOG_INFO("The start-failure page was opened");
    }
    else
    {
        LOG_WARNING("The browser program could not be started for the start-failure page");
    }

    return exitCode;
}

/// Lets a stop function that was handed out outlive the server: after `close` it does nothing,
/// so a late call (a test, a signal handler of the caller) never touches a destroyed server.
class StopGate {
  public:
    explicit StopGate(HttpServer& aServer) : mServer(&aServer) {}

    /// Asks the server to stop; nothing happens after `close`.
    void stop() {
        const std::lock_guard<std::mutex> lock(mMutex);

        if (mServer != nullptr)
        {
            mServer->stop();
        }
    }

    /// Called before the server is destroyed.
    void close() {
        const std::lock_guard<std::mutex> lock(mMutex);
        mServer = nullptr;
    }

  private:
    std::mutex mMutex;
    HttpServer* mServer; ///< Null after `close`.
};

/// Closes its gate when it goes out of scope, also when `run` throws.
class StopGateCloser {
  public:
    explicit StopGateCloser(StopGate& aGate) : mGate(aGate) {}
    ~StopGateCloser() {
        mGate.close();
    }

    StopGateCloser(const StopGateCloser&) = delete;
    StopGateCloser& operator=(const StopGateCloser&) = delete;

  private:
    StopGate& mGate;
};

} // namespace

int StartServer(const EnvLookup& aEnv,
                std::ostream& aOut,
                std::ostream& aErr,
                const ServerHooks& aHooks) {
    const std::variant<ServerConfig, std::string> read = readServerConfig(aEnv);

    if (std::holds_alternative<std::string>(read))
    {
        return failStart(aEnv, aHooks, aErr, std::get<std::string>(read));
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
        options.mNoLoginLimit = aHooks.mNoLoginLimit;
        options.mIdleLimit = aHooks.mIdleLimit;

        // Docker (TBR_LAUNCH_FILE set): the developer's server goes on when the tab closes.
        options.mClosedPageLimit =
            config.mLaunchFile.has_value() ? std::chrono::milliseconds{0} : aHooks.mClosedPageLimit;

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
            embeddedStaticFiles(),
            buildVersion(),
            ProfileStore::forUser(aEnv));

        // A TBR_LAUNCH_FILE folder is made by the caller (Docker: scripts/run.sh).
        if (!config.mLaunchFile.has_value())
        {
            makePrivateFolder(launchFile.parent_path());
        }

        writeLaunchFile(launchFile, makeLaunchPage(server->port(), linkToken));

        // A page of an earlier failed start is out of date now.
        if (!config.mLaunchFile.has_value())
        {
            removeStartFailurePage(aEnv);
        }
    } catch (const std::exception& error)
    {
        // Every failure above throws a text without a path or a value.
        return failStart(aEnv, aHooks, aErr, error.what());
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

    // Declared after `server`, so it closes the gate before the server is destroyed.
    const std::shared_ptr<StopGate> gate = std::make_shared<StopGate>(*server);
    const StopGateCloser gateCloser(*gate);

    if (aHooks.mOnListening)
    {
        aHooks.mOnListening(port, [gate]() { gate->stop(); });
    }

    server->run([&routes](const HttpRequest& aRequest) { return routes->handle(aRequest); },
                [&routes](const HttpRequest& aHeader) { return routes->bodyPolicy(aHeader); });

    std::error_code ignored;
    std::filesystem::remove(launchFile, ignored);

    LOG_INFO("Server stopped");

    return 0;
}

} // namespace taxbroker
