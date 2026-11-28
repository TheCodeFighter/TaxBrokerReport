#include "server/browser_open.hpp"
#include "server/server.hpp"
#include "server_test_support.hpp"

#include <gtest/gtest.h>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/stat.h>

namespace {
using namespace taxbroker;
using namespace taxbroker::servertest;

using namespace std::chrono_literals;

/// The longest wait for a server that must stop by itself; a limit is 50 to 600 ms, the rest is
/// room for a slow machine (Valgrind).
constexpr std::chrono::milliseconds kStopWait{10'000};

/// The permission bits of a file or folder.
unsigned modeOf(const std::filesystem::path& aPath) {
    struct stat info{};
    EXPECT_EQ(::stat(aPath.c_str(), &info), 0);

    return info.st_mode & 0777U;
}

/// True when the text holds the part.
bool contains(const std::string& aText, const std::string& aPart) {
    return aText.find(aPart) != std::string::npos;
}

/// A request with the session cookie, the client header and the origin: what the page sends.
std::string sessionRequestWith(const std::string& aMethod,
                               std::uint16_t aPort,
                               const std::string& aTarget,
                               const std::string& aSessionToken) {
    return aMethod + " " + aTarget +
           " HTTP/1.1\r\n"
           "Host: 127.0.0.1:" +
           std::to_string(aPort) +
           "\r\n"
           "Origin: http://127.0.0.1:" +
           std::to_string(aPort) +
           "\r\n"
           "X-TaxBroker: 1\r\n"
           "Content-Length: 0\r\n"
           "Cookie: tbr_session_" +
           std::to_string(aPort) + "=" + aSessionToken + "\r\n\r\n";
}

class ServerStopTest : public ::testing::Test {
  protected:
    void SetUp() override {
        mFolder = std::filesystem::temp_directory_path() /
                  (std::string("tbr_stop_") +
                   ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(mFolder);
        std::filesystem::create_directories(mFolder);

        // The server logs through the default logger; keep its lines to check them.
        mPreviousLogger = spdlog::default_logger();
        std::shared_ptr<spdlog::sinks::ostream_sink_mt> sink =
            std::make_shared<spdlog::sinks::ostream_sink_mt>(mLog);
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("server_stop_test", sink));
    }

    void TearDown() override {
        spdlog::set_default_logger(mPreviousLogger);

        std::error_code ignored;
        std::filesystem::remove_all(mFolder, ignored);
    }

    /// Environment with the state and runtime folders inside the test folder.
    EnvLookup standardEnv() const {
        return envOf({{"XDG_STATE_HOME", (mFolder / "state").string()},
                      {"XDG_RUNTIME_DIR", (mFolder / "run").string()},
                      {"HOME", mFolder.string()}});
    }

    /// The same, with a port that is not a number.
    EnvLookup badPortEnv() const {
        return envOf({{"XDG_STATE_HOME", (mFolder / "state").string()},
                      {"XDG_RUNTIME_DIR", (mFolder / "run").string()},
                      {"HOME", mFolder.string()},
                      {"TBR_SERVER_PORT", "abc"}});
    }

    std::filesystem::path launchFile() const {
        return mFolder / "run" / "taxbroker" / "launch.html";
    }

    std::filesystem::path failurePage() const {
        return mFolder / "run" / "taxbroker" / "start_failed.html";
    }

    /// Logs in with the link of the launch file and returns the session token.
    std::string login(const RunningServer& aServer) const {
        const std::string link = tokenIn(readAll(launchFile()));
        Client client(aServer.port());
        client.send(loginRequest(aServer.port(), link));
        const std::optional<Response> response = client.readResponse();

        EXPECT_TRUE(response.has_value());

        if (!response.has_value())
        {
            return "";
        }

        const std::string cookie = std::string((*response)[http::field::set_cookie]);
        const std::string prefix = "tbr_session_" + std::to_string(aServer.port()) + "=";

        return cookie.substr(prefix.size(), 64);
    }

    std::filesystem::path mFolder;
    std::ostringstream mLog;
    std::shared_ptr<spdlog::logger> mPreviousLogger;
};

/// An opener that only records the file it was asked to open.
BrowserOpener recordingOpener(std::vector<std::filesystem::path>& aOpened) {
    return [&aOpened](const std::filesystem::path& aFile) {
        aOpened.push_back(aFile);

        return true;
    };
}

// ---- Quit route

TEST_F(ServerStopTest, QuitFromThePageStopsTheServerAfterTheAnswerWasWritten) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(), recordingOpener(opened));

    ASSERT_GT(server.port(), 0);

    const std::uint16_t port = server.port();
    const std::string session = login(server);

    Client client(port);
    client.send(sessionRequestWith("POST", port, "/api/quit", session));
    const std::optional<Response> answer = client.readResponse();

    // The page gets its answer before the server stops.
    ASSERT_TRUE(answer.has_value());

    EXPECT_EQ(answer->result_int(), 200U);
    EXPECT_EQ(answer->body(), R"({"stopping":true})");

    ASSERT_TRUE(server.waitForExit(kStopWait));

    EXPECT_EQ(server.stop(), 0);
    EXPECT_FALSE(std::filesystem::exists(launchFile()));
    EXPECT_FALSE(Client(port).connected());
    EXPECT_TRUE(contains(mLog.str(), "Stopped from the page"));
    EXPECT_TRUE(contains(mLog.str(), "Server stopped"));
}

TEST_F(ServerStopTest, QuitWithoutTheSessionCookieIsRefusedAndTheServerKeepsServing) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(), recordingOpener(opened));

    ASSERT_GT(server.port(), 0);

    const std::uint16_t port = server.port();
    const std::string session = login(server);

    Client client(port);
    client.send("POST /api/quit HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                "\r\nOrigin: http://127.0.0.1:" + std::to_string(port) +
                "\r\nX-TaxBroker: 1\r\nContent-Length: 0\r\n\r\n");
    const std::optional<Response> refused = client.readResponse();

    ASSERT_TRUE(refused.has_value());

    EXPECT_EQ(refused->result_int(), 401U);

    client.send(sessionRequestWith("GET", port, "/api/version", session));
    const std::optional<Response> next = client.readResponse();

    ASSERT_TRUE(next.has_value());

    EXPECT_EQ(next->result_int(), 200U);
}

TEST_F(ServerStopTest, QuitWithoutTheClientHeaderIsRefusedAndTheServerKeepsServing) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(), recordingOpener(opened));

    ASSERT_GT(server.port(), 0);

    const std::uint16_t port = server.port();
    const std::string session = login(server);

    Client client(port);
    client.send("POST /api/quit HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                "\r\nOrigin: http://127.0.0.1:" + std::to_string(port) +
                "\r\nContent-Length: 0\r\nCookie: tbr_session_" + std::to_string(port) + "=" +
                session + "\r\n\r\n");
    const std::optional<Response> refused = client.readResponse();

    ASSERT_TRUE(refused.has_value());

    EXPECT_EQ(refused->result_int(), 403U);

    client.send(sessionRequestWith("GET", port, "/api/version", session));
    const std::optional<Response> next = client.readResponse();

    ASSERT_TRUE(next.has_value());

    EXPECT_EQ(next->result_int(), 200U);
}

TEST_F(ServerStopTest, GetQuitIsNotAllowedAndTheServerKeepsServing) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(), recordingOpener(opened));

    ASSERT_GT(server.port(), 0);

    const std::uint16_t port = server.port();
    const std::string session = login(server);

    Client client(port);
    client.send(sessionRequestWith("GET", port, "/api/quit", session));
    const std::optional<Response> refused = client.readResponse();

    ASSERT_TRUE(refused.has_value());

    EXPECT_EQ(refused->result_int(), 405U);

    client.send(sessionRequestWith("GET", port, "/api/version", session));
    const std::optional<Response> next = client.readResponse();

    ASSERT_TRUE(next.has_value());

    EXPECT_EQ(next->result_int(), 200U);
}

// ---- Automatic stop

TEST_F(ServerStopTest, ServerStopsWhenNobodyLogsInWithinTheLimit) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(), recordingOpener(opened), kMaxInputFileBytes, 50ms, 0ms);

    ASSERT_TRUE(server.waitForExit(kStopWait));

    EXPECT_EQ(server.stop(), 0);
    EXPECT_FALSE(std::filesystem::exists(launchFile()));
    EXPECT_TRUE(contains(mLog.str(), "Stopped: nobody opened the link"));
}

TEST_F(ServerStopTest, LoginEndsTheNoLoginWait) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(), recordingOpener(opened), kMaxInputFileBytes, 1000ms, 0ms);

    ASSERT_GT(server.port(), 0);

    (void)login(server);

    // Well after the 1000 ms limit the server still runs: without a login it would have stopped.
    EXPECT_FALSE(server.waitForExit(1500ms));
    EXPECT_FALSE(contains(mLog.str(), "Stopped: nobody opened the link"));
}

TEST_F(ServerStopTest, ServerStopsWhenItIsNotUsedForTheIdleLimit) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(), recordingOpener(opened), kMaxInputFileBytes, 0ms, 50ms);

    ASSERT_GT(server.port(), 0);

    (void)login(server);

    ASSERT_TRUE(server.waitForExit(kStopWait));

    EXPECT_EQ(server.stop(), 0);
    EXPECT_TRUE(contains(mLog.str(), "Stopped: not used for a while"));
}

TEST_F(ServerStopTest, EveryRequestWithTheSessionRestartsTheIdleWait) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(), recordingOpener(opened), kMaxInputFileBytes, 0ms, 600ms);

    ASSERT_GT(server.port(), 0);

    const std::uint16_t port = server.port();
    const std::string session = login(server);

    Client client(port);
    std::vector<unsigned> statuses;

    // Eight requests 25 ms apart take about 200 ms and never leave a gap of 600 ms.
    for (int count = 0; count < 8; ++count)
    {
        client.send(sessionRequestWith("GET", port, "/api/version", session));
        const std::optional<Response> answer = client.readResponse();

        ASSERT_TRUE(answer.has_value());

        statuses.push_back(answer->result_int());
        std::this_thread::sleep_for(25ms);
    }

    EXPECT_EQ(statuses, std::vector<unsigned>(8, 200U));
    EXPECT_FALSE(contains(mLog.str(), "Stopped: not used for a while"));

    // After the last request the server is idle and stops.
    ASSERT_TRUE(server.waitForExit(kStopWait));

    EXPECT_EQ(server.stop(), 0);
    EXPECT_TRUE(contains(mLog.str(), "Stopped: not used for a while"));
}

// ---- Start-failure page

TEST_F(ServerStopTest, FailedNativeStartWritesAndOpensThePageWithTheError) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(badPortEnv(), recordingOpener(opened));

    EXPECT_EQ(server.stop(), 1);
    EXPECT_EQ(server.err(), "Error: TBR_SERVER_PORT must be a whole number from 0 to 65535.\n");

    ASSERT_TRUE(std::filesystem::exists(failurePage()));

    EXPECT_EQ(modeOf(failurePage()), 0600U);
    EXPECT_EQ(modeOf(failurePage().parent_path()), 0700U);

    const std::string page = readAll(failurePage());

    EXPECT_TRUE(contains(page, "TaxBrokerReport se ni zagnal"));
    EXPECT_TRUE(contains(page, "TaxBrokerReport did not start"));
    EXPECT_TRUE(contains(page, "TBR_SERVER_PORT must be a whole number from 0 to 65535."));
    EXPECT_TRUE(contains(page, "mailto:tax.brokerage.report@gmail.com"));
    EXPECT_TRUE(contains(page, "Version 2.0.0"));
    EXPECT_FALSE(contains(page, "<script"));
    EXPECT_FALSE(contains(page, mFolder.string())); // no path of this computer

    ASSERT_EQ(opened.size(), 1U);

    EXPECT_EQ(opened[0], failurePage());
    EXPECT_TRUE(contains(mLog.str(), "The start-failure page was opened"));
}

TEST_F(ServerStopTest, FailedStartInDockerWritesNoPageAndOpensNothing) {
    std::vector<std::filesystem::path> opened;
    const EnvLookup env = envOf({{"XDG_STATE_HOME", (mFolder / "state").string()},
                                 {"XDG_RUNTIME_DIR", (mFolder / "run").string()},
                                 {"HOME", mFolder.string()},
                                 {"TBR_SERVER_PORT", "abc"},
                                 {"TBR_LAUNCH_FILE", (mFolder / "launch.html").string()}});
    RunningServer server(env, recordingOpener(opened));

    EXPECT_EQ(server.stop(), 1);
    EXPECT_FALSE(std::filesystem::exists(failurePage()));
    EXPECT_TRUE(opened.empty());
}

TEST_F(ServerStopTest, GoodStartRemovesTheOldPage) {
    std::filesystem::create_directories(failurePage().parent_path());
    std::ofstream(failurePage()) << "an old page";

    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(), recordingOpener(opened));

    ASSERT_GT(server.port(), 0);

    EXPECT_FALSE(std::filesystem::exists(failurePage()));
    EXPECT_TRUE(std::filesystem::exists(launchFile()));
}

TEST_F(ServerStopTest, PageGoesToTheStateFolderWithoutARuntimeFolder) {
    std::vector<std::filesystem::path> opened;
    const EnvLookup env = envOf({{"HOME", mFolder.string()}, {"TBR_SERVER_PORT", "abc"}});
    RunningServer server(env, recordingOpener(opened));

    EXPECT_EQ(server.stop(), 1);

    const std::filesystem::path page =
        mFolder / ".local" / "state" / "taxbroker" / "start_failed.html";

    EXPECT_TRUE(std::filesystem::exists(page));
    ASSERT_EQ(opened.size(), 1U);

    EXPECT_EQ(opened[0], page);
}

TEST_F(ServerStopTest, PageThatIsASymbolicLinkIsNotFollowed) {
    const std::filesystem::path target = mFolder / "target.txt";
    std::ofstream(target) << "original";

    std::filesystem::create_directories(failurePage().parent_path());
    std::filesystem::create_symlink(target, failurePage());

    std::vector<std::filesystem::path> opened;
    RunningServer server(badPortEnv(), recordingOpener(opened));

    EXPECT_EQ(server.stop(), 1);
    EXPECT_EQ(readAll(target), "original");

    // The link in the launch folder is refused; the page goes to the state folder instead.
    const std::filesystem::path statePage = mFolder / "state" / "taxbroker" / "start_failed.html";

    ASSERT_EQ(opened.size(), 1U);

    EXPECT_EQ(opened[0], statePage);
    EXPECT_TRUE(std::filesystem::exists(statePage));
}

TEST_F(ServerStopTest, PageGoesToTheStateFolderWhenTheLaunchFolderCannotBeMade) {
    // "run/taxbroker" is a file, so the launch folder cannot be made.
    std::filesystem::create_directories(mFolder / "run");
    std::ofstream(mFolder / "run" / "taxbroker") << "not a folder";

    std::vector<std::filesystem::path> opened;
    RunningServer server(badPortEnv(), recordingOpener(opened));

    EXPECT_EQ(server.stop(), 1);

    const std::filesystem::path statePage = mFolder / "state" / "taxbroker" / "start_failed.html";

    ASSERT_EQ(opened.size(), 1U);

    EXPECT_EQ(opened[0], statePage);
    EXPECT_EQ(modeOf(statePage), 0600U);
    EXPECT_TRUE(contains(readAll(statePage), "TaxBrokerReport did not start"));
}

// ---- Stop when the last page closed (R22)

/// The tab message with the session cookie; aOpen is the flag.
std::string tabRequestWith(std::uint16_t aPort,
                           const std::string& aSessionToken,
                           const std::string& aId,
                           bool aOpen) {
    const std::string body = "{\"id\":\"" + aId + "\",\"open\":" + (aOpen ? "true" : "false") + "}";

    return "POST /api/tab HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(aPort) +
           "\r\nOrigin: http://127.0.0.1:" + std::to_string(aPort) +
           "\r\nX-TaxBroker: 1\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nCookie: tbr_session_" + std::to_string(aPort) + "=" +
           aSessionToken + "\r\n\r\n" + body;
}

TEST_F(ServerStopTest, ServerStopsWhenTheLastPageClosedAndNoPageComesBack) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(),
                         recordingOpener(opened),
                         kMaxInputFileBytes,
                         0ms,
                         0ms,
                         50ms);

    ASSERT_GT(server.port(), 0);

    const std::uint16_t port = server.port();
    const std::string session = login(server);

    Client client(port);
    client.send(tabRequestWith(port, session, "tab-1", true));

    ASSERT_TRUE(client.readResponse().has_value());

    client.send(tabRequestWith(port, session, "tab-1", false));
    const std::optional<Response> closed = client.readResponse();

    ASSERT_TRUE(closed.has_value());

    EXPECT_EQ(closed->result_int(), 204U);

    ASSERT_TRUE(server.waitForExit(kStopWait));

    EXPECT_EQ(server.stop(), 0);
    EXPECT_FALSE(std::filesystem::exists(launchFile()));
    EXPECT_TRUE(contains(mLog.str(), "Stopped: the page was closed"));
}

TEST_F(ServerStopTest, AnOpenMessageAfterTheCloseKeepsTheServer) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(),
                         recordingOpener(opened),
                         kMaxInputFileBytes,
                         0ms,
                         0ms,
                         1000ms);

    ASSERT_GT(server.port(), 0);

    const std::uint16_t port = server.port();
    const std::string session = login(server);

    Client client(port);
    client.send(tabRequestWith(port, session, "tab-1", true));

    ASSERT_TRUE(client.readResponse().has_value());

    client.send(tabRequestWith(port, session, "tab-1", false));

    ASSERT_TRUE(client.readResponse().has_value());

    // A reload: the new page has a new id and says it is open within the 1000 ms.
    client.send(tabRequestWith(port, session, "tab-2", true));

    ASSERT_TRUE(client.readResponse().has_value());

    // Well past the 1000 ms of the wait the server still runs.
    EXPECT_FALSE(server.waitForExit(1500ms));
    EXPECT_FALSE(contains(mLog.str(), "Stopped: the page was closed"));
}

TEST_F(ServerStopTest, ARequestWithoutAnOpenMessageDoesNotKeepTheServer) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(),
                         recordingOpener(opened),
                         kMaxInputFileBytes,
                         0ms,
                         0ms,
                         300ms);

    ASSERT_GT(server.port(), 0);

    const std::uint16_t port = server.port();
    const std::string session = login(server);

    Client client(port);
    client.send(tabRequestWith(port, session, "tab-1", true));

    ASSERT_TRUE(client.readResponse().has_value());

    client.send(tabRequestWith(port, session, "tab-1", false));

    ASSERT_TRUE(client.readResponse().has_value());

    // A page that is closing may still get the answer of a run: a request with the session that
    // is no open message must not end the wait.
    client.send(sessionRequestWith("GET", port, "/api/version", session));
    const std::optional<Response> answer = client.readResponse();

    ASSERT_TRUE(answer.has_value());

    EXPECT_EQ(answer->result_int(), 200U);

    ASSERT_TRUE(server.waitForExit(kStopWait));

    EXPECT_EQ(server.stop(), 0);
    EXPECT_TRUE(contains(mLog.str(), "Stopped: the page was closed"));
}

TEST_F(ServerStopTest, TheServerKeepsRunningWhileAnotherPageIsOpen) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(),
                         recordingOpener(opened),
                         kMaxInputFileBytes,
                         0ms,
                         0ms,
                         50ms);

    ASSERT_GT(server.port(), 0);

    const std::uint16_t port = server.port();
    const std::string session = login(server);

    Client client(port);

    for (const std::string id : {"tab-1", "tab-2"})
    {
        client.send(tabRequestWith(port, session, id, true));

        ASSERT_TRUE(client.readResponse().has_value());
    }

    client.send(tabRequestWith(port, session, "tab-1", false));

    ASSERT_TRUE(client.readResponse().has_value());

    // One page is still open: well after the 50 ms limit the server runs.
    EXPECT_FALSE(server.waitForExit(500ms));

    client.send(tabRequestWith(port, session, "tab-2", false));

    ASSERT_TRUE(client.readResponse().has_value());
    ASSERT_TRUE(server.waitForExit(kStopWait));
    EXPECT_EQ(server.stop(), 0);
}

TEST_F(ServerStopTest, ClosedPageLimitOfZeroNeverStopsTheServer) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(), recordingOpener(opened));

    ASSERT_GT(server.port(), 0);

    const std::uint16_t port = server.port();
    const std::string session = login(server);

    Client client(port);
    client.send(tabRequestWith(port, session, "tab-1", true));

    ASSERT_TRUE(client.readResponse().has_value());

    client.send(tabRequestWith(port, session, "tab-1", false));

    ASSERT_TRUE(client.readResponse().has_value());

    EXPECT_FALSE(server.waitForExit(300ms));
}

TEST_F(ServerStopTest, DockerStartIgnoresTheClosedPageLimit) {
    std::vector<std::filesystem::path> opened;
    const std::filesystem::path dockerLaunchFile = mFolder / "launch.html";
    const EnvLookup env = envOf({{"XDG_STATE_HOME", (mFolder / "state").string()},
                                 {"HOME", mFolder.string()},
                                 {"TBR_LAUNCH_FILE", dockerLaunchFile.string()}});
    RunningServer server(env, recordingOpener(opened), kMaxInputFileBytes, 0ms, 0ms, 50ms);

    ASSERT_GT(server.port(), 0);

    const std::uint16_t port = server.port();
    const std::string link = tokenIn(readAll(dockerLaunchFile));
    Client loginClient(port);
    loginClient.send(loginRequest(port, link));
    const std::optional<Response> loggedIn = loginClient.readResponse();

    ASSERT_TRUE(loggedIn.has_value());

    const std::string cookie = std::string((*loggedIn)[http::field::set_cookie]);
    const std::string session =
        cookie.substr(("tbr_session_" + std::to_string(port) + "=").size(), 64);

    loginClient.send(tabRequestWith(port, session, "tab-1", true));

    ASSERT_TRUE(loginClient.readResponse().has_value());

    loginClient.send(tabRequestWith(port, session, "tab-1", false));

    ASSERT_TRUE(loginClient.readResponse().has_value());

    // The limit is 50 ms, but a Docker start ignores it.
    EXPECT_FALSE(server.waitForExit(400ms));
}

// ---- Start-failure page: the paths that need an unusual setup

TEST_F(ServerStopTest, FailedStartLogsAWarningWhenTheBrowserProgramCannotBeStarted) {
    const BrowserOpener failingOpener = [](const std::filesystem::path&) { return false; };
    RunningServer server(badPortEnv(), failingOpener);

    EXPECT_EQ(server.stop(), 1);
    EXPECT_TRUE(std::filesystem::exists(failurePage()));
    EXPECT_TRUE(contains(mLog.str(),
                         "The browser program could not be started for the start-failure page"));
}

// ---- The stop function that StartServer hands out

TEST_F(ServerStopTest, StopFunctionCalledAfterTheServerEndedDoesNothing) {
    std::function<void()> stopFunction;
    ServerHooks hooks;
    hooks.mOpener = [](const std::filesystem::path&) { return true; };
    hooks.mHandleSignals = false;
    hooks.mMaxFileBytes = kMaxInputFileBytes;
    hooks.mNoLoginLimit = 50ms;
    hooks.mOnListening = [&stopFunction](std::uint16_t, std::function<void()> aStop) {
        stopFunction = std::move(aStop);
    };

    std::ostringstream out;
    std::ostringstream err;

    // Nobody logs in, so StartServer returns by itself after 50 ms and its server is destroyed.
    EXPECT_EQ(StartServer(standardEnv(), out, err, hooks), 0);

    ASSERT_TRUE(static_cast<bool>(stopFunction));

    // Memory checkers (Valgrind in CI) fail this call when it still touches the server.
    EXPECT_NO_THROW(stopFunction());
}

} // namespace
