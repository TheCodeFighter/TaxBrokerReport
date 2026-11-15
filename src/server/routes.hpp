#pragma once

#include "http_message.hpp"
#include "request_guard.hpp"
#include "run_routes.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace taxbroker {

/// The routes of the local server: the browser login, the placeholder page and the run API. They
/// have no sockets, so the tests call `handle` directly. The calls come from one thread.
class Routes {
  public:
    /// Returns the current time; the tests pass a fake clock.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// The link token is issued now. The launch file is deleted after the login. aMaxFileBytes is
    /// the longest file of an upload.
    Routes(std::uint16_t aPort,
           std::string aLinkToken,
           std::string aSessionToken,
           std::filesystem::path aLaunchFile,
           Clock aClock,
           std::size_t aMaxFileBytes);

    /// The body limit for a request whose header is read and whose body is not: the upload limit
    /// for `POST /api/run` that passes the guard, else `kDefaultBodyLimit`. It has no side effect
    /// (no token is redeemed), so a caller without a session cannot make the server read a large
    /// body.
    [[nodiscard]] BodyPolicy bodyPolicy(const HttpRequest& aHeaderOnly) const;

    /// Checks the request in the fixed order of `docs/architecture.md`, then routes it.
    [[nodiscard]] HttpResponse handle(const HttpRequest& aRequest);

  private:
    /// What the checks of the guard decided.
    enum class Target {
        Refused, ///< The request fails a check; mRefusal is the answer.
        Page,    ///< The placeholder page or an unknown path: no login needed.
        Login,   ///< `POST /api/session`: the one API call without a session.
        Api,     ///< An API call with a valid session.
    };

    /// What the guard decided, and the answer when it refused the request.
    struct Verdict {
        Target mTarget{};      ///< Always set by the creator.
        HttpResponse mRefusal; ///< The answer for Refused; empty otherwise.
        /// The fixed word for the log (`host_not_allowed`, `no_session`, ...); empty unless
        /// Refused.
        std::string_view mReason{};
    };

    /// Steps 1 to 6 of the guard. It changes nothing.
    [[nodiscard]] Verdict check(const HttpRequest& aRequest) const;

    HttpResponse login(const HttpRequest& aRequest);

    std::uint16_t mPort;
    LinkToken mLinkToken;
    std::string mSessionToken;
    std::filesystem::path mLaunchFile;
    Clock mClock;
    RunRoutes mRunRoutes;
};

} // namespace taxbroker
