#pragma once

#include "http_message.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace taxbroker {

/// The limits and signal handling of the HTTP server.
struct HttpServerOptions {
    std::chrono::milliseconds
        mTimeout{};             ///< Longest wait to read, write or between chunks; always set.
    std::size_t mHeaderLimit{}; ///< Largest request header block in bytes; always set.
    std::uint64_t mBodyLimit{}; ///< Body limit without a body policy; always set.
    bool mHandleSignals{};      ///< True: SIGINT, SIGTERM and (not Windows) SIGHUP stop `run`.
    /// `run` stops when no response with `mSessionUsed` was written this long after it started;
    /// zero means no limit. Always set.
    std::chrono::milliseconds mNoLoginLimit{};
    /// `run` stops when this long passes after a response with `mSessionUsed` and no other one
    /// follows; zero means no limit. Always set.
    std::chrono::milliseconds mIdleLimit{};
    /// `run` stops when this long passes after a response with `mLastPageClosed` and no response
    /// with `mPageOpened` follows (a reload makes one); zero means no limit. Always set.
    std::chrono::milliseconds mClosedPageLimit{};
};

/// A small HTTP/1.1 server on Boost.Beast. One thread runs every connection, so the handler is
/// never called twice at the same time; a browser can keep several connections open meanwhile.
/// The header is read first. The body limit then comes from the body policy, so a large body is
/// read only for a request the guard accepts. The body is read as long as bytes keep coming: the
/// timeout of the options is the wait for the header and the idle time between two chunks.
class HttpServer {
  public:
    /// Turns a request into a response. A thrown exception becomes a 500 without detail.
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    /// Decides the body limit of a request from its header alone (the body is empty). An empty
    /// policy function gives every request the `mBodyLimit` of the options and a passed guard.
    using BodyPolicyFunction = std::function<BodyPolicy(const HttpRequest&)>;

    /// Binds and listens at once. aHost is an IP address or `localhost`; port 0 picks a free one.
    /// Throws `std::runtime_error` (no path or value in the text) when that is not possible.
    HttpServer(const std::string& aHost, std::uint16_t aPort, const HttpServerOptions& aOptions);

    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// The port that is bound; the real one when 0 was asked for.
    [[nodiscard]] std::uint16_t port() const;

    /// Serves on the calling thread until `stop`, a signal, the quit of a response
    /// (`mStopAfterWrite`, after it is written) or one of the three limits. Closes the listening
    /// socket at the end. A request with `Expect: 100-continue` gets the `100 Continue` only when
    /// the policy says the guard passed.
    void run(Handler aHandler, BodyPolicyFunction aBodyPolicy = {});

    /// Ends `run`. Safe to call from any thread, once `run` was called or is about to be.
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace taxbroker
