#include "server/browser_open.hpp"
#include "server/http_server.hpp"
#include "server/server.hpp"
#include "server_test_support.hpp"

#include <gtest/gtest.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace taxbroker;

using namespace taxbroker::servertest;

/// The permission bits of a file or folder.
unsigned modeOf(const std::filesystem::path& aPath) {
    struct stat info{};
    EXPECT_EQ(::stat(aPath.c_str(), &info), 0);

    return info.st_mode & 0777U;
}

/// An HttpServer on its own thread.
class RunningHttpServer {
  public:
    RunningHttpServer(const HttpServerOptions& aOptions,
                      HttpServer::Handler aHandler,
                      HttpServer::BodyPolicyFunction aBodyPolicy = {})
        : mServer("127.0.0.1", 0, aOptions) {
        mThread =
            std::thread([this, aHandler, aBodyPolicy]() { mServer.run(aHandler, aBodyPolicy); });
    }

    ~RunningHttpServer() {
        mServer.stop();
        mThread.join();
    }

    [[nodiscard]] std::uint16_t port() const {
        return mServer.port();
    }

  private:
    HttpServer mServer;
    std::thread mThread;
};

HttpServerOptions testOptions() {
    HttpServerOptions options;
    options.mTimeout = std::chrono::seconds(30);
    options.mHeaderLimit = 8 * 1024;
    options.mBodyLimit = 1024 * 1024;
    options.mHandleSignals = false;

    return options;
}

HttpResponse okHandler(const HttpRequest&) {
    HttpResponse response;
    response.mStatus = 200;
    response.mBody = "ok";

    return response;
}

/// Sets PATH for one test and restores it at the end.
class PathOverride {
  public:
    explicit PathOverride(const std::string& aValue) {
        const char* old = std::getenv("PATH");
        mHadOld = old != nullptr;
        mOld = mHadOld ? old : "";
        ::setenv("PATH", aValue.c_str(), 1);
    }

    PathOverride(const PathOverride&) = delete;
    PathOverride& operator=(const PathOverride&) = delete;

    ~PathOverride() {
        if (mHadOld)
        {
            ::setenv("PATH", mOld.c_str(), 1);
        }
        else
        {
            ::unsetenv("PATH");
        }
    }

  private:
    std::string mOld;
    bool mHadOld = false;
};

/// Uses up every free file handle of the process. `releaseOne` frees one for a test socket; the
/// destructor frees all and restores the limit.
class UsedUpFileHandles {
  public:
    UsedUpFileHandles() {
        // Lower the limit to the highest open handle, so the gaps below it are all there is.
        const int highest = std::accumulate(
            std::filesystem::directory_iterator("/proc/self/fd"),
            std::filesystem::directory_iterator(),
            0,
            [](int aHighest, const std::filesystem::directory_entry& aEntry) {
                return std::max(aHighest, std::stoi(aEntry.path().filename().string()));
            });

        ::getrlimit(RLIMIT_NOFILE, &mOldLimit);

        rlimit lowered = mOldLimit;
        lowered.rlim_cur = static_cast<rlim_t>(highest) + 1;
        ::setrlimit(RLIMIT_NOFILE, &lowered);

        for (;;)
        {
            const int handle = ::open("/dev/null", O_RDONLY);

            if (handle < 0)
            {
                mIsFull = errno == EMFILE;
                break;
            }

            mHandles.push_back(handle);
        }
    }

    UsedUpFileHandles(const UsedUpFileHandles&) = delete;
    UsedUpFileHandles& operator=(const UsedUpFileHandles&) = delete;

    ~UsedUpFileHandles() {
        releaseAll();
    }

    /// True when the loop ended because no handle was free (not for another reason).
    [[nodiscard]] bool isFull() const {
        return mIsFull;
    }

    void releaseOne() {
        ::close(mHandles.back());
        mHandles.pop_back();
    }

    void releaseAll() {
        for (const int handle : mHandles)
        {
            ::close(handle);
        }

        mHandles.clear();
        ::setrlimit(RLIMIT_NOFILE, &mOldLimit);
    }

  private:
    std::vector<int> mHandles;
    rlimit mOldLimit{};
    bool mIsFull = false; ///< Set by the constructor.
};

/// CPU time of all threads of the process, in seconds.
double processCpuSeconds() {
    timespec time{};
    ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time);

    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) / 1e9;
}

class ServerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        mFolder = std::filesystem::temp_directory_path() /
                  (std::string("tbr_server_") +
                   ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(mFolder);
        std::filesystem::create_directories(mFolder);

        // The server logs through the default logger; keep its lines to check them.
        mPreviousLogger = spdlog::default_logger();
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(mLog);
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("server_test", sink));
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

    std::filesystem::path launchFile() const {
        return mFolder / "run" / "taxbroker" / "launch.html";
    }

    std::filesystem::path mFolder;
    std::ostringstream mLog;
    std::shared_ptr<spdlog::logger> mPreviousLogger;
};

// ---- Start (R1, R2, R6, R19)

TEST_F(ServerTest, ListensOnTheLoopbackOnly) {
    RunningServer server(standardEnv(), [](const std::filesystem::path&) { return true; });

    ASSERT_GT(server.port(), 0); // port 0 was asked for, so a random free port was read back

    const Client loopback(server.port(), "127.0.0.1");
    const Client otherLoopbackAddress(server.port(), "127.0.0.2");

    EXPECT_TRUE(loopback.connected());
    EXPECT_FALSE(otherLoopbackAddress.connected()); // bound to 127.0.0.1 only, not to 127.0.0.0/8
}

TEST_F(ServerTest, OpensTheLaunchFileInTheBrowserAndPrintsTheLinkToo) {
    std::vector<std::filesystem::path> opened;
    RunningServer server(standardEnv(), [&opened](const std::filesystem::path& aFile) {
        opened.push_back(aFile);

        return true;
    });

    ASSERT_GT(server.port(), 0);
    ASSERT_EQ(opened.size(), 1U);

    EXPECT_EQ(opened[0], launchFile());
    EXPECT_EQ(opened[0].string().find("token"), std::string::npos); // R6: no secret in the argument
    EXPECT_EQ(modeOf(launchFile()), 0600U);

    const std::string content = readAll(launchFile());
    const std::string link = tokenIn(content);
    const std::string expectedPrefix =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/#token=";

    EXPECT_NE(content.find(expectedPrefix), std::string::npos);

    // xdg-open may fail later (no desktop), so the link is printed although the opener started.
    ASSERT_EQ(link.size(), 64U);

    EXPECT_EQ(server.out(),
              "Open http://127.0.0.1:" + std::to_string(server.port()) + "/#token=" + link +
                  " in your browser (it works once, for 2 minutes).\n");
}

TEST_F(ServerTest, ListeningSocketAndLogFileAreNotPassedToTheBrowserProgram) {
    // A fake xdg-open lists its own open files into a pipe, so the test waits without a sleep.
    const std::filesystem::path bin = mFolder / "bin";
    std::filesystem::create_directories(bin);

    const std::filesystem::path pipe = mFolder / "handles.pipe";
    ASSERT_EQ(::mkfifo(pipe.c_str(), 0600), 0);

    const std::filesystem::path script = bin / "xdg-open";
    std::ofstream(script) << "#!/bin/sh\nls -l /proc/$$/fd > '" << pipe.string() << "'\n";
    std::filesystem::permissions(script, std::filesystem::perms::owner_all);

    // The fake comes first: a real xdg-open would never write to the pipe.
    const PathOverride path(bin.string() + ":/usr/bin:/bin");

    // The app logs into a file with this sink; the test logger alone would open no file.
    const std::filesystem::path logFile = mFolder / "server.log";
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(
        "server_file_test",
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFile.string())));

    RunningServer server(standardEnv(), openInBrowser);

    ASSERT_GT(server.port(), 0);

    std::ifstream reader(pipe);
    std::ostringstream listing;
    listing << reader.rdbuf();

    // A browser that inherited the listening socket would keep the port after the server ended,
    // and one that inherited the log file would keep it open for writing.
    EXPECT_NE(listing.str().find("->"), std::string::npos); // the listing is not empty
    EXPECT_EQ(listing.str().find("socket:"), std::string::npos);
    EXPECT_EQ(listing.str().find("server.log"), std::string::npos);
}

TEST_F(ServerTest, LoginOverTcpGivesTheCookieAndDeletesTheLaunchFile) {
    RunningServer server(standardEnv(), [](const std::filesystem::path&) { return true; });

    ASSERT_GT(server.port(), 0);

    const std::string link = tokenIn(readAll(launchFile()));

    ASSERT_EQ(link.size(), 64U);

    Client client(server.port());
    client.send(loginRequest(server.port(), link));
    const std::optional<Response> login = client.readResponse();

    ASSERT_TRUE(login.has_value());

    EXPECT_EQ(login->result_int(), 204U);
    EXPECT_FALSE(std::filesystem::exists(launchFile()));

    const std::string cookie = std::string((*login)[http::field::set_cookie]);
    const std::string prefix = "tbr_session_" + std::to_string(server.port()) + "=";

    ASSERT_EQ(cookie.rfind(prefix, 0), 0U);

    const std::string session = cookie.substr(prefix.size(), 64);
    client.send(sessionRequest(server.port(), session));
    const std::optional<Response> check = client.readResponse();

    ASSERT_TRUE(check.has_value());

    EXPECT_EQ(check->result_int(), 204U);
}

TEST_F(ServerTest, PrintsTheLinkWhenTheBrowserCannotBeStarted) {
    RunningServer server(standardEnv(), [](const std::filesystem::path&) { return false; });

    ASSERT_GT(server.port(), 0);

    const std::string link = tokenIn(readAll(launchFile()));

    ASSERT_EQ(link.size(), 64U);

    EXPECT_EQ(server.out(),
              "Open http://127.0.0.1:" + std::to_string(server.port()) + "/#token=" + link +
                  " in your browser (it works once, for 2 minutes).\n");
}

TEST_F(ServerTest, LaunchFileFromTheEnvironmentIsNotOpenedAndTheLinkIsPrinted) {
    std::filesystem::create_directories(mFolder / "launch");
    const std::filesystem::path file = mFolder / "launch" / "launch.html";
    int openCalls = 0;

    RunningServer server(envOf({{"TBR_LAUNCH_FILE", file.string()}, {"HOME", mFolder.string()}}),
                         [&openCalls](const std::filesystem::path&) {
                             ++openCalls;

                             return true;
                         });

    ASSERT_GT(server.port(), 0);

    const std::string link = tokenIn(readAll(file));

    ASSERT_EQ(link.size(), 64U);

    EXPECT_EQ(openCalls, 0); // Docker: the host opens the file
    EXPECT_EQ(modeOf(file), 0600U);
    EXPECT_EQ(server.out(),
              "Open http://127.0.0.1:" + std::to_string(server.port()) + "/#token=" + link +
                  " in your browser (it works once, for 2 minutes).\n");
}

TEST_F(ServerTest, LaunchFileThatIsASymbolicLinkStopsTheStart) {
    std::filesystem::create_directories(mFolder / "launch");
    const std::filesystem::path target = mFolder / "launch" / "target.html";
    const std::filesystem::path link = mFolder / "launch" / "launch.html";
    std::ofstream(target) << "do not touch";
    std::filesystem::create_symlink(target, link);

    RunningServer server(envOf({{"TBR_LAUNCH_FILE", link.string()}, {"HOME", mFolder.string()}}),
                         [](const std::filesystem::path&) { return true; });

    EXPECT_EQ(server.stop(), 1);
    EXPECT_EQ(server.err(), "Error: cannot write the launch file\n"); // no path in the text
    EXPECT_EQ(readAll(target), "do not touch");
}

TEST_F(ServerTest, LaunchFileIsInTheStateFolderWithoutARuntimeFolder) {
    RunningServer server(
        envOf({{"XDG_STATE_HOME", (mFolder / "state").string()}, {"HOME", mFolder.string()}}),
        [](const std::filesystem::path&) { return true; });

    ASSERT_GT(server.port(), 0);

    const std::filesystem::path file = mFolder / "state" / "taxbroker" / "launch.html";

    EXPECT_TRUE(std::filesystem::exists(file));
    EXPECT_EQ(modeOf(file.parent_path()), 0700U);
}

TEST_F(ServerTest, ConfigurationErrorStopsTheStart) {
    RunningServer server(envOf({{"TBR_SERVER_PORT", "80"}, {"HOME", mFolder.string()}}),
                         [](const std::filesystem::path&) { return true; });

    EXPECT_EQ(server.stop(), 1);
    EXPECT_EQ(server.err(),
              "Error: Port 80 is not supported: browsers leave it out of the Host header. Use "
              "another port.\n");
}

TEST_F(ServerTest, PortInUseStopsTheStart) {
    RunningServer first(standardEnv(), [](const std::filesystem::path&) { return true; });

    ASSERT_GT(first.port(), 0);

    RunningServer second(envOf({{"XDG_STATE_HOME", (mFolder / "state2").string()},
                                {"XDG_RUNTIME_DIR", (mFolder / "run2").string()},
                                {"TBR_SERVER_PORT", std::to_string(first.port())},
                                {"HOME", mFolder.string()}}),
                         [](const std::filesystem::path&) { return true; });

    EXPECT_EQ(second.stop(), 1);
    EXPECT_EQ(second.err(), "Error: cannot listen: the port is in use or not allowed\n");
}

TEST_F(ServerTest, NonLoopbackHostLogsOneWarning) {
    RunningServer server(envOf({{"TBR_SERVER_HOST", "0.0.0.0"},
                                {"XDG_STATE_HOME", (mFolder / "state").string()},
                                {"XDG_RUNTIME_DIR", (mFolder / "run").string()},
                                {"HOME", mFolder.string()}}),
                         [](const std::filesystem::path&) { return true; });

    ASSERT_GT(server.port(), 0);

    EXPECT_EQ(server.stop(), 0);

    const std::string log = mLog.str();
    const std::string warning =
        "The server listens on a non-loopback address; publish its port on 127.0.0.1 only.";

    ASSERT_NE(log.find(warning), std::string::npos);

    EXPECT_EQ(log.find(warning, log.find(warning) + 1), std::string::npos); // only once
}

TEST_F(ServerTest, HostThatIsNotAnAddressStopsTheStart) {
    RunningServer server(envOf({{"TBR_SERVER_HOST", "not-an-address"}, {"HOME", mFolder.string()}}),
                         [](const std::filesystem::path&) { return true; });

    EXPECT_EQ(server.stop(), 1);
    EXPECT_EQ(server.err(), "Error: TBR_SERVER_HOST is not an IP address.\n");
}

TEST_F(ServerTest, Ipv6HostStopsTheStart) {
    RunningServer server(envOf({{"TBR_SERVER_HOST", "::1"}, {"HOME", mFolder.string()}}),
                         [](const std::filesystem::path&) { return true; });

    EXPECT_EQ(server.stop(), 1);
    EXPECT_EQ(server.err(),
              "Error: TBR_SERVER_HOST must be an IPv4 address or localhost: the link always uses "
              "127.0.0.1.\n");
}

// ---- Stop (R19)

TEST_F(ServerTest, StopDeletesTheLaunchFileAndClosesThePort) {
    RunningServer server(standardEnv(), [](const std::filesystem::path&) { return true; });

    ASSERT_GT(server.port(), 0);
    ASSERT_TRUE(std::filesystem::exists(launchFile()));

    const std::uint16_t port = server.port();

    EXPECT_EQ(server.stop(), 0);

    EXPECT_FALSE(std::filesystem::exists(launchFile()));
    EXPECT_FALSE(Client(port).connected());
}

// ---- Log (R4, R13)

TEST_F(ServerTest, LogHoldsNeitherTokenNorPath) {
    std::string session;
    std::string link;

    {
        RunningServer server(standardEnv(), [](const std::filesystem::path&) { return false; });

        ASSERT_GT(server.port(), 0);

        link = tokenIn(readAll(launchFile()));

        Client client(server.port());
        client.send(loginRequest(server.port(), link));
        const std::optional<Response> login = client.readResponse();

        ASSERT_TRUE(login.has_value());

        const std::string cookie = std::string((*login)[http::field::set_cookie]);
        session = cookie.substr(cookie.find('=') + 1, 64);
    }

    const std::string log = mLog.str();

    ASSERT_EQ(link.size(), 64U);
    ASSERT_EQ(session.size(), 64U);

    EXPECT_NE(log.find("Server listening on 127.0.0.1:"), std::string::npos);
    EXPECT_NE(log.find("Browser login done"), std::string::npos);
    EXPECT_NE(log.find("The browser program could not be started"), std::string::npos);
    EXPECT_NE(log.find("Server stopped"), std::string::npos);
    EXPECT_EQ(log.find(link), std::string::npos);
    EXPECT_EQ(log.find(session), std::string::npos);
    EXPECT_EQ(log.find(mFolder.string()), std::string::npos);
    EXPECT_EQ(log.find("/tmp"), std::string::npos);
}

TEST_F(ServerTest, ABrowserProgramThatStartsIsLoggedWithoutTheFileName) {
    {
        RunningServer server(standardEnv(), [](const std::filesystem::path&) { return true; });

        ASSERT_GT(server.port(), 0);
    }

    const std::string log = mLog.str();

    EXPECT_NE(log.find("The browser program was started"), std::string::npos);
    EXPECT_EQ(log.find("could not be started"), std::string::npos);
    EXPECT_EQ(log.find("launch.html"), std::string::npos);
}

// ---- HTTP layer

TEST_F(ServerTest, SecondConnectionIsServedWhileTheFirstStaysOpen) {
    RunningHttpServer server(testOptions(), okHandler);

    Client first(server.port());
    first.send(getRequest(server.port(), "/"));
    const std::optional<Response> firstResponse = first.readResponse();

    ASSERT_TRUE(firstResponse.has_value());

    EXPECT_EQ(firstResponse->result_int(), 200U);

    // The first connection is kept alive and idle; the second must not wait for it.
    Client second(server.port());
    second.send(getRequest(server.port(), "/"));
    const std::optional<Response> secondResponse = second.readResponse();

    ASSERT_TRUE(secondResponse.has_value());

    EXPECT_EQ(secondResponse->result_int(), 200U);
    EXPECT_EQ(secondResponse->body(), "ok");

    first.send(getRequest(server.port(), "/again"));
    const std::optional<Response> firstAgain = first.readResponse();

    ASSERT_TRUE(firstAgain.has_value());

    EXPECT_EQ(firstAgain->result_int(), 200U);
}

TEST_F(ServerTest, ConnectionThatSendsHalfAHeaderIsClosedAfterTheTimeout) {
    HttpServerOptions options = testOptions();
    options.mTimeout = std::chrono::milliseconds(200);
    RunningHttpServer server(options, okHandler);

    Client stalled(server.port());
    stalled.send("GET / HTTP/1.1\r\nHost");

    EXPECT_TRUE(stalled.readsUntilClosed());

    Client next(server.port());
    next.send(getRequest(server.port(), "/"));
    const std::optional<Response> response = next.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 200U);
}

TEST_F(ServerTest, GarbageGetsBadRequestWithTheSecurityHeaders) {
    RunningHttpServer server(testOptions(), okHandler);

    Client client(server.port());
    client.send("hello\r\n\r\n");
    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 400U);
    EXPECT_EQ((*response)["X-Content-Type-Options"], "nosniff");
    EXPECT_EQ((*response)["Cache-Control"], "no-store");

    Client next(server.port());
    next.send(getRequest(server.port(), "/"));
    const std::optional<Response> nextResponse = next.readResponse();

    ASSERT_TRUE(nextResponse.has_value());

    EXPECT_EQ(nextResponse->result_int(), 200U);
}

TEST_F(ServerTest, HeaderBlockOfNineKiBIsRefused) {
    RunningHttpServer server(testOptions(), okHandler);

    Client client(server.port());
    client.send("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Padding: " + std::string(9 * 1024, 'a') +
                "\r\n\r\n");
    const std::optional<Response> response = client.readResponse();

    // The server answers 431 and closes; if the close wins the race, the client sees no response.
    if (response.has_value())
    {
        EXPECT_EQ(response->result_int(), 431U);
    }

    Client next(server.port());
    next.send(getRequest(server.port(), "/"));
    const std::optional<Response> nextResponse = next.readResponse();

    ASSERT_TRUE(nextResponse.has_value());

    EXPECT_EQ(nextResponse->result_int(), 200U);
}

TEST_F(ServerTest, BodyAboveTheLimitIsRefused) {
    HttpServerOptions options = testOptions();
    options.mBodyLimit = 10;
    RunningHttpServer server(options, okHandler);

    Client client(server.port());
    client.send("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 11\r\n\r\n01234567890");
    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 413U);
    EXPECT_NE(mLog.str().find("Request refused: 413 body_over_the_limit (limit 10)"),
              std::string::npos);
}

/// The body policy of the body-limit tests: a request with a Cookie header passed the guard and may
/// send 2'500'000 bytes; any other may send 1 MiB.
BodyPolicy cookieBodyPolicy(const HttpRequest& aHeader) {
    const bool hasCookie =
        std::any_of(aHeader.mHeaders.begin(), aHeader.mHeaders.end(), [](const HttpField& aField) {
            return aField.mName == "Cookie";
        });

    return hasCookie ? BodyPolicy{2'500'000, true} : BodyPolicy{1024 * 1024, false};
}

/// 401 for a request without a Cookie header, else 200 with the length of the body as text.
HttpResponse cookieHandler(const HttpRequest& aRequest) {
    const bool hasCookie =
        std::any_of(aRequest.mHeaders.begin(),
                    aRequest.mHeaders.end(),
                    [](const HttpField& aField) { return aField.mName == "Cookie"; });

    HttpResponse response;
    response.mStatus = hasCookie ? 200 : 401;
    response.mBody = std::to_string(aRequest.mBody.size());

    return response;
}

TEST_F(ServerTest, LargeBodyWithoutTheGuardIsRefusedBeforeItIsRead) {
    RunningHttpServer server(testOptions(), cookieHandler, cookieBodyPolicy);

    // Only the header is sent: the 413 comes without a body to read.
    Client client(server.port());
    client.send("POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2000000\r\n\r\n");
    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 413U);
    EXPECT_TRUE(client.readsUntilClosed());
}

TEST_F(ServerTest, ChunkedBodyAboveTheLimitIsRefused) {
    // A chunked body has no declared length, so the limit is checked while the body is read.
    HttpServerOptions options = testOptions();
    options.mBodyLimit = 10;
    RunningHttpServer server(options, okHandler);

    Client client(server.port());
    client.send("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                "b\r\n01234567890\r\n0\r\n\r\n");
    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 413U);
    EXPECT_NE(mLog.str().find("Request refused: 413 unreadable_request"), std::string::npos);
}

TEST_F(ServerTest, FailingBodyPolicyGivesInternalErrorWithoutDetail) {
    RunningHttpServer server(testOptions(), okHandler, [](const HttpRequest&) -> BodyPolicy {
        throw std::runtime_error("secret detail of /home/someone");
    });

    Client client(server.port());
    client.send(getRequest(server.port(), "/"));
    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 500U);
    EXPECT_EQ(response->body(), "");
    // The verb, the step and the kind of the exception; never its text.
    EXPECT_NE(mLog.str().find("A request handler failed: GET body_policy (std::runtime_error)"),
              std::string::npos);
    EXPECT_EQ(mLog.str().find("secret"), std::string::npos);
}

TEST_F(ServerTest, SmallBodyWithoutTheGuardStillGetsItsAnswer) {
    RunningHttpServer server(testOptions(), cookieHandler, cookieBodyPolicy);

    Client client(server.port());
    client.send("POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 10\r\n\r\n0123456789");
    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 401U);
}

/// A body policy for a request that failed the guard: the 401 and the word `session_wrong`.
BodyPolicy refusingBodyPolicy(const HttpRequest&) {
    BodyPolicy policy{1024 * 1024, false};

    policy.mRefusal = makeResponse(401);
    policy.mReason = "session_wrong";

    return policy;
}

TEST_F(ServerTest, LargeBodyOfARefusedRequestGetsTheGuardAnswerAndItsReason) {
    RunningHttpServer server(testOptions(), okHandler, refusingBodyPolicy);

    // Only the header is sent: the answer is the guard's 401, not a 413, and the handler is not
    // called (it would answer 200).
    Client client(server.port());
    client.send("POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2000000\r\n\r\n");
    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 401U);
    EXPECT_TRUE(client.readsUntilClosed());

    const std::string log = mLog.str();

    EXPECT_NE(log.find("Request refused: 401 session_wrong"), std::string::npos);
    EXPECT_EQ(log.find("body_over_the_limit"), std::string::npos);
}

TEST_F(ServerTest, ChunkedBodyOfARefusedRequestGetsTheGuardAnswerAndItsReason) {
    RunningHttpServer server(testOptions(), okHandler, refusingBodyPolicy);

    // A chunked body has no declared length, so its size is not known before it is read.
    Client client(server.port());
    client.send("POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                "4\r\nabcd\r\n");
    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 401U);
    EXPECT_NE(mLog.str().find("Request refused: 401 session_wrong"), std::string::npos);
}

TEST_F(ServerTest, SmallBodyOfARefusedRequestStillReachesTheHandler) {
    RunningHttpServer server(testOptions(), okHandler, refusingBodyPolicy);

    // A small body is read, and the handler refuses it (here it answers 200 to show it ran). The
    // guard's answer is sent at once only for a body that is large or of unknown size.
    Client client(server.port());
    client.send("POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 4\r\n\r\nabcd");
    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 200U);
    EXPECT_EQ(mLog.str().find("Request refused"), std::string::npos);
}

TEST_F(ServerTest, RefusedRequestWithoutABodyReachesTheHandler) {
    RunningHttpServer server(testOptions(), okHandler, refusingBodyPolicy);

    Client client(server.port());
    client.send(getRequest(server.port(), "/"));
    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 200U);
}

TEST_F(ServerTest, LargeBodyWithTheGuardReachesTheHandler) {
    RunningHttpServer server(testOptions(), cookieHandler, cookieBodyPolicy);

    Client client(server.port());
    client.send("POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: a=b\r\n"
                "Content-Length: 2000000\r\n\r\n" +
                std::string(2000000, 'x'));
    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 200U);
    EXPECT_EQ(response->body(), "2000000");
}

TEST_F(ServerTest, SlowBodyIsReadWhileBytesKeepComing) {
    HttpServerOptions options = testOptions();
    options.mTimeout = std::chrono::milliseconds(200);
    RunningHttpServer server(options, cookieHandler, cookieBodyPolicy);

    // Four chunks 150 ms apart take 600 ms, three times the idle timeout, but no wait is longer
    // than the timeout.
    Client client(server.port());
    client.send("POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: a=b\r\n"
                "Content-Length: 4\r\n\r\n");

    for (const char* chunk : {"a", "b", "c", "d"})
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        client.send(chunk);
    }

    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 200U);
    EXPECT_EQ(response->body(), "4");
}

TEST_F(ServerTest, BodyThatStopsIsClosedAfterTheIdleTimeout) {
    HttpServerOptions options = testOptions();
    options.mTimeout = std::chrono::milliseconds(200);

    {
        RunningHttpServer server(options, cookieHandler, cookieBodyPolicy);

        // Four bytes are promised and one arrives.
        Client client(server.port());
        client.send("POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: a=b\r\n"
                    "Content-Length: 4\r\n\r\na");

        EXPECT_TRUE(client.readsUntilClosed());
    }

    // The timeout closes the socket before the handler writes its line, so the log is read after
    // the server has stopped.
    EXPECT_NE(mLog.str().find("Request not finished: timeout"), std::string::npos);
}

TEST_F(ServerTest, BodyThatIsCutByTheClientIsLogged) {
    RunningHttpServer server(testOptions(), cookieHandler, cookieBodyPolicy);

    // Four bytes are promised, one arrives, then the client ends its stream.
    Client client(server.port());
    client.send("POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: a=b\r\n"
                "Content-Length: 4\r\n\r\na");
    client.finishSending();

    EXPECT_TRUE(client.readsUntilClosed());
    EXPECT_NE(mLog.str().find("Request not finished: connection_closed"), std::string::npos);
}

TEST_F(ServerTest, AnIdleConnectionThatEndsWritesNoLine) {
    RunningHttpServer server(testOptions(), cookieHandler, cookieBodyPolicy);

    // Nothing is sent: the connection ends while the server waits for a header.
    Client client(server.port());
    client.finishSending();

    EXPECT_TRUE(client.readsUntilClosed());
    EXPECT_EQ(mLog.str().find("Request not finished"), std::string::npos);
}

TEST_F(ServerTest, ExpectContinueGetsTheContinueOnlyWhenTheGuardPassed) {
    RunningHttpServer server(testOptions(), cookieHandler, cookieBodyPolicy);

    // With the guard passed: the server answers 100 before the body is sent.
    Client withGuard(server.port());
    withGuard.send("POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: a=b\r\n"
                   "Expect: 100-continue\r\nContent-Length: 4\r\n\r\n");
    const std::optional<Response> interim = withGuard.readResponse();

    ASSERT_TRUE(interim.has_value());

    EXPECT_EQ(interim->result_int(), 100U);

    withGuard.send("abcd");
    const std::optional<Response> final = withGuard.readResponse();

    ASSERT_TRUE(final.has_value());

    EXPECT_EQ(final->result_int(), 200U);
    EXPECT_EQ(final->body(), "4");

    // Without the guard: no 100, the first and only answer is the 401.
    Client withoutGuard(server.port());
    withoutGuard.send("POST /upload HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                      "Expect: 100-continue\r\nContent-Length: 4\r\n\r\nabcd");
    const std::optional<Response> refused = withoutGuard.readResponse();

    ASSERT_TRUE(refused.has_value());

    EXPECT_EQ(refused->result_int(), 401U);
}

TEST_F(ServerTest, FailingHandlerGivesInternalErrorWithoutDetail) {
    RunningHttpServer server(testOptions(), [](const HttpRequest&) -> HttpResponse {
        throw std::runtime_error("secret detail of /home/someone");
    });

    Client client(server.port());
    client.send(getRequest(server.port(), "/"));
    const std::optional<Response> response = client.readResponse();

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->result_int(), 500U);
    EXPECT_EQ(response->body(), "");
    EXPECT_EQ((*response)["Cache-Control"], "no-store");
    EXPECT_NE(mLog.str().find("A request handler failed: GET handler (std::runtime_error)"),
              std::string::npos);
    EXPECT_EQ(mLog.str().find("secret"), std::string::npos);
}

TEST_F(ServerTest, AcceptWaitsAndThenRecoversWhenTheProcessRunsOutOfFileHandles) {
    RunningHttpServer server(testOptions(), okHandler);

    // Every handle is in use, then one is freed for the test's own socket. The server cannot
    // accept this connection (it has no handle for it), but the connection stays queued.
    UsedUpFileHandles handles;

    ASSERT_TRUE(handles.isFull());

    handles.releaseOne();

    const int client = ::socket(AF_INET, SOCK_STREAM, 0);

    ASSERT_GE(client, 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server.port());
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    const int connected = ::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address));

    ASSERT_EQ(connected, 0);

    const std::string request = getRequest(server.port(), "/");

    ASSERT_EQ(::send(client, request.data(), request.size(), 0),
              static_cast<ssize_t>(request.size()));

    // A waiting connection that cannot be accepted must not make the server spin. A busy loop
    // would use about 0.2 s of CPU in these 0.2 s; waiting between tries uses almost none. This
    // short wait is the only way to see the difference.
    const double before = processCpuSeconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const double used = processCpuSeconds() - before;

    EXPECT_LT(used, 0.1);

    // With handles free again, the next try accepts the connection and answers it.
    handles.releaseAll();

    timeval timeout{5, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::string received;
    char buffer[512];

    while (received.find("\r\n\r\nok") == std::string::npos)
    {
        const ssize_t count = ::recv(client, buffer, sizeof(buffer), 0);

        if (count <= 0)
        {
            break;
        }

        received.append(buffer, static_cast<std::size_t>(count));
    }

    ::close(client);

    EXPECT_EQ(received.rfind("HTTP/1.1 200", 0), 0U);

    // Several tries failed in the 0.2 s above, but the log has one warning for the series.
    const std::string log = mLog.str();
    const std::size_t first = log.find("Accept failed: ");

    ASSERT_NE(first, std::string::npos);

    EXPECT_EQ(log.find("Accept failed: ", first + 1), std::string::npos);
}

TEST_F(ServerTest, SigtermStopsTheServerWhenSignalsAreHandled) {
    HttpServerOptions options = testOptions();
    options.mHandleSignals = true;
    HttpServer server("127.0.0.1", 0, options);
    std::atomic<bool> finished{false};

    std::thread thread([&server, &finished]() {
        server.run(okHandler);
        finished = true;
    });

    // The answer proves that run() is serving, so the signal handler is registered already.
    Client client(server.port());
    client.send(getRequest(server.port(), "/"));

    ASSERT_TRUE(client.readResponse().has_value());
    ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);

    thread.join();

    EXPECT_TRUE(finished);
}

TEST_F(ServerTest, LocalhostIsAnAcceptedListenName) {
    HttpServer server("localhost", 0, testOptions());

    EXPECT_GT(server.port(), 0);
}

} // namespace
