#pragma once

#include "routes.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace taxbroker {

/// The limits and signal handling of the HTTP server.
struct HttpServerOptions {
    std::chrono::milliseconds mTimeout{}; ///< Longest wait to read or write; always set.
    std::size_t mHeaderLimit{};           ///< Largest request header block in bytes; always set.
    std::uint64_t mBodyLimit{};           ///< Largest request body in bytes; always set.
    bool mHandleSignals{};                ///< True: SIGINT and SIGTERM stop `run`.
};

/// A small HTTP/1.1 server on Boost.Beast. One thread runs every connection, so the handler is
/// never called twice at the same time; a browser can keep several connections open meanwhile.
class HttpServer {
  public:
    /// Turns a request into a response. A thrown exception becomes a 500 without detail.
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    /// Binds and listens at once. aHost is an IP address or `localhost`; port 0 picks a free one.
    /// Throws `std::runtime_error` (no path or value in the text) when that is not possible.
    HttpServer(const std::string& aHost, std::uint16_t aPort, const HttpServerOptions& aOptions);

    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// The port that is bound; the real one when 0 was asked for.
    [[nodiscard]] std::uint16_t port() const;

    /// Serves on the calling thread until `stop` or a signal. Closes the listening socket at the
    /// end.
    void run(Handler aHandler);

    /// Ends `run`. Safe to call from any thread, once `run` was called or is about to be.
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace taxbroker
