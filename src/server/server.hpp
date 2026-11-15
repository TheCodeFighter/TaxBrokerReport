#pragma once

#include "browser_open.hpp"
#include "server_config.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>

namespace taxbroker {

/// What the caller can replace; the tests pass their own.
struct ServerHooks {
    BrowserOpener mOpener; ///< Opens the launch file; always set.
    /// Called on the server thread just before it serves: the port and a `stop` function that
    /// may be called from any thread. May be empty.
    std::function<void(std::uint16_t, std::function<void()>)> mOnListening;
    bool mHandleSignals{}; ///< True: SIGINT and SIGTERM stop the server. Always set.
    /// The longest file of an upload in bytes; a longer one is refused. Always set (the server
    /// passes `kMaxInputFileBytes`; a test passes a small one).
    std::size_t mMaxFileBytes{};
};

/// Starts the local server and serves until it is stopped. Prints the browser link to aOut (also
/// when a browser was started, as it may still fail) and an error line to aErr. Returns 0 after a
/// stop, 1 when the start failed.
[[nodiscard]] int StartServer(const EnvLookup& aEnv,
                              std::ostream& aOut,
                              std::ostream& aErr,
                              const ServerHooks& aHooks);

} // namespace taxbroker
