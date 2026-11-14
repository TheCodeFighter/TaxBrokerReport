#pragma once

#include "request_guard.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace taxbroker {

/// One header of a request or a response.
struct HttpField {
    std::string mName;  ///< Header name, as sent.
    std::string mValue; ///< Header value.
};

/// A request as the routes see it. It holds no socket.
struct HttpRequest {
    std::string mMethod;             ///< `GET`, `POST`, ...; always set by the creator.
    std::string mTarget;             ///< Path and query, as sent.
    std::vector<HttpField> mHeaders; ///< All headers, in the order sent.
    std::string mBody;               ///< The body; empty when there is none.
};

/// A response before it is written. Every one carries the security headers.
struct HttpResponse {
    unsigned mStatus{};              ///< HTTP status code; always set by the creator.
    std::vector<HttpField> mHeaders; ///< Security headers first, then the route's own.
    std::string mBody;               ///< The body; empty when there is none.
};

/// An error response with the security headers and no body detail: 400, 413, 431 and 500 come
/// from the HTTP layer, the rest from the routes.
[[nodiscard]] HttpResponse makeErrorResponse(unsigned aStatus);

/// The routes of the local server: the browser login and the placeholder page. They have no
/// sockets, so the tests call `handle` directly. The calls come from one thread.
class Routes {
  public:
    /// Returns the current time; the tests pass a fake clock.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// The link token is issued now. The launch file is deleted after the login.
    Routes(std::uint16_t aPort,
           std::string aLinkToken,
           std::string aSessionToken,
           std::filesystem::path aLaunchFile,
           Clock aClock);

    /// Checks the request in the fixed order of `docs/architecture.md`, then routes it.
    [[nodiscard]] HttpResponse handle(const HttpRequest& aRequest);

  private:
    HttpResponse login(const HttpRequest& aRequest);

    std::uint16_t mPort;
    LinkToken mLinkToken;
    std::string mSessionToken;
    std::filesystem::path mLaunchFile;
    Clock mClock;
};

} // namespace taxbroker
