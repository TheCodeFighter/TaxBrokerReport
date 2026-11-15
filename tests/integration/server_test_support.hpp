#pragma once

// Helpers of the integration tests that talk to a real server over TCP: a blocking client and the
// server on its own thread. They are shared by ServerTest (the HTTP layer) and ServerRunTest (the
// run API through the server).

#include "server/browser_open.hpp"
#include "server/server.hpp"

#include "taxbroker/input_file.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace taxbroker::servertest {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

using Response = http::response<http::string_body>;

/// The whole content of a file.
inline std::string readAll(const std::filesystem::path& aPath) {
    std::ifstream file(aPath, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();

    return content.str();
}

/// An environment made of a map, so the test names every variable it sets.
inline EnvLookup envOf(std::map<std::string, std::string> aValues) {
    return [aValues](std::string_view aName) -> std::optional<std::string> {
        const auto found = aValues.find(std::string(aName));

        if (found == aValues.end())
        {
            return std::nullopt;
        }

        return found->second;
    };
}

/// A blocking client for one connection.
class Client {
  public:
    /// Connects; check `connected()`.
    explicit Client(std::uint16_t aPort, const std::string& aAddress = "127.0.0.1")
        : mSocket(mContext) {
        beast::error_code error;
        mSocket.connect(tcp::endpoint(asio::ip::make_address(aAddress), aPort), error);
        mConnected = !error;
    }

    [[nodiscard]] bool connected() const {
        return mConnected;
    }

    void send(const std::string& aRaw) {
        asio::write(mSocket, asio::buffer(aRaw));
    }

    /// Closes the sending side only, so the server reads the end of the stream.
    void finishSending() {
        beast::error_code ignored;
        mSocket.shutdown(tcp::socket::shutdown_send, ignored);
    }

    /// Reads one response; nothing when the connection closed or broke before it was complete.
    std::optional<Response> readResponse() {
        beast::error_code error;
        Response response;
        http::read(mSocket, mBuffer, response, error);

        if (error)
        {
            return std::nullopt;
        }

        return response;
    }

    /// True when reading ends with a closed or reset connection and no more data.
    bool readsUntilClosed() {
        beast::error_code error;
        char scratch[256];

        do
        {
            mSocket.read_some(asio::buffer(scratch), error);
        } while (!error);

        return error == asio::error::eof || error == asio::error::connection_reset;
    }

  private:
    asio::io_context mContext;
    tcp::socket mSocket;
    beast::flat_buffer mBuffer;
    bool mConnected = false; ///< Set by the constructor.
};

/// A request without a body.
inline std::string getRequest(std::uint16_t aPort, const std::string& aTarget) {
    return "GET " + aTarget + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(aPort) + "\r\n\r\n";
}

/// The login request with the given token in the body.
inline std::string loginRequest(std::uint16_t aPort, const std::string& aToken) {
    const std::string body = "{\"token\":\"" + aToken + "\"}";

    return "POST /api/session HTTP/1.1\r\n"
           "Host: 127.0.0.1:" +
           std::to_string(aPort) +
           "\r\n"
           "Origin: http://127.0.0.1:" +
           std::to_string(aPort) +
           "\r\n"
           "X-TaxBroker: 1\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

/// The check request with the session cookie.
inline std::string sessionRequest(std::uint16_t aPort, const std::string& aSessionToken) {
    return "GET /api/session HTTP/1.1\r\n"
           "Host: 127.0.0.1:" +
           std::to_string(aPort) +
           "\r\n"
           "X-TaxBroker: 1\r\n"
           "Cookie: tbr_session_" +
           std::to_string(aPort) + "=" + aSessionToken + "\r\n\r\n";
}

/// The 64 hex characters after `#token=` in a text.
inline std::string tokenIn(const std::string& aText) {
    const std::string marker = "#token=";
    const std::size_t start = aText.find(marker);

    if (start == std::string::npos)
    {
        return "";
    }

    return aText.substr(start + marker.size(), 64);
}

/// StartServer on its own thread, until `stop` or the destructor.
class RunningServer {
  public:
    RunningServer(const EnvLookup& aEnv,
                  const BrowserOpener& aOpener,
                  std::size_t aMaxFileBytes = kMaxInputFileBytes) {
        mThread = std::thread([this, aEnv, aOpener, aMaxFileBytes]() {
            ServerHooks hooks;
            hooks.mOpener = aOpener;
            hooks.mHandleSignals = false;
            hooks.mMaxFileBytes = aMaxFileBytes;
            hooks.mOnListening = [this](std::uint16_t aPort, std::function<void()> aStop) {
                const std::lock_guard<std::mutex> lock(mMutex);
                mPort = aPort;
                mStop = std::move(aStop);
                mChanged.notify_all();
            };

            const int exitCode = StartServer(aEnv, mOut, mErr, hooks);

            const std::lock_guard<std::mutex> lock(mMutex);
            mExitCode = exitCode;
            mFinished = true;
            mChanged.notify_all();
        });

        std::unique_lock<std::mutex> lock(mMutex);
        mChanged.wait(lock, [this]() { return mPort != 0 || mFinished; });
    }

    ~RunningServer() {
        (void)stop();
    }

    /// The listening port; 0 when the start failed.
    [[nodiscard]] std::uint16_t port() const {
        return mPort;
    }

    /// Stops the server, waits for it and returns the exit code.
    int stop() {
        if (mThread.joinable())
        {
            if (mStop)
            {
                mStop();
            }

            mThread.join();
        }

        return mExitCode;
    }

    [[nodiscard]] std::string out() const {
        return mOut.str();
    }

    [[nodiscard]] std::string err() const {
        return mErr.str();
    }

  private:
    std::thread mThread;
    std::mutex mMutex;
    std::condition_variable mChanged;
    std::uint16_t mPort = 0; ///< 0 until the server listens.
    std::function<void()> mStop;
    bool mFinished = false; ///< True when StartServer returned.
    int mExitCode = -1;     ///< Set when StartServer returned.
    std::ostringstream mOut;
    std::ostringstream mErr;
};

} // namespace taxbroker::servertest
