#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace taxbroker {

/// Looks up an environment variable. The tests pass a map instead of the real environment.
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

/// The lookup of the real process environment.
[[nodiscard]] EnvLookup systemEnvironment();

/// The server options that come from the environment.
struct ServerConfig {
    std::string mHost{};   ///< Address to listen on; always set by the creator.
    std::uint16_t mPort{}; ///< Port to listen on; 0 means a random free port. Always set.
    std::optional<std::filesystem::path> mLaunchFile; ///< `TBR_LAUNCH_FILE`; absolute when set.
};

/// Reads `TBR_SERVER_HOST` (default `127.0.0.1`; an IPv6 address is refused), `TBR_SERVER_PORT`
/// (default 0) and `TBR_LAUNCH_FILE`. Returns the options, or one error line that names the
/// variable but never its value.
[[nodiscard]] std::variant<ServerConfig, std::string> readServerConfig(const EnvLookup& aEnv);

/// True for the exact names `127.0.0.1` and `localhost`.
[[nodiscard]] bool isLoopbackHost(std::string_view aHost);

} // namespace taxbroker
