#pragma once

#include "browser_open.hpp"
#include "request_guard.hpp"
#include "server_config.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>

namespace taxbroker {

/// How long the server waits after the link's lifetime before it stops for lack of a login.
inline constexpr std::chrono::seconds kNoLoginGrace{60};

/// The server stops when nobody has logged in this long after the start. It is longer than the
/// link, so a login right at the end of the link's lifetime still finds a running server.
inline constexpr std::chrono::seconds kNoLoginLimit = kLinkTokenLifetime + kNoLoginGrace;

/// The server stops this long after the last request with a session (a long reading break).
inline constexpr std::chrono::minutes kIdleLimit{60};

/// The server stops this long after the last open page closed. Long enough for a reload to open
/// the page again.
inline constexpr std::chrono::seconds kClosedPageLimit{10};

/// What the caller can replace; the tests pass their own.
struct ServerHooks {
    BrowserOpener mOpener; ///< Opens the launch file; always set.
    /// Called on the server thread just before it serves: the port and a `stop` function that
    /// may be called from any thread, also after the server ended (then it does nothing). May be
    /// empty.
    std::function<void(std::uint16_t, std::function<void()>)> mOnListening;
    /// True: SIGINT, SIGTERM and (not on Windows) SIGHUP stop the server. Always set.
    bool mHandleSignals{};
    /// The longest file of an upload in bytes; a longer one is refused. Always set (the server
    /// passes `kMaxInputFileBytes`; a test passes a small one).
    std::size_t mMaxFileBytes{};
    /// The server stops when nobody has logged in this long after the start; zero: no limit.
    /// Always set (the server passes `kNoLoginLimit`).
    std::chrono::milliseconds mNoLoginLimit{};
    /// The server stops this long after the last request with a session; zero: no limit. Always
    /// set (the server passes `kIdleLimit`).
    std::chrono::milliseconds mIdleLimit{};
    /// The server stops this long after the last open page closed, unless a page says it is open
    /// again (a reload); zero: no limit. Used only without `TBR_LAUNCH_FILE` (a Docker run goes on
    /// when the tab closes). Always set (the server passes `kClosedPageLimit`).
    std::chrono::milliseconds mClosedPageLimit{};
};

/// Starts the local server and serves until it is stopped. Prints the browser link to aOut (also
/// when a browser was started, as it may still fail) and an error line to aErr. Returns 0 after a
/// stop, 1 when the start failed.
[[nodiscard]] int StartServer(const EnvLookup& aEnv,
                              std::ostream& aOut,
                              std::ostream& aErr,
                              const ServerHooks& aHooks);

} // namespace taxbroker
