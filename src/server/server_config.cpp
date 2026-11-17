#include "server_config.hpp"

#include <charconv>
#include <cstdlib>
#include <system_error>

namespace taxbroker {

namespace {

constexpr std::string_view kPortError = "TBR_SERVER_PORT must be a whole number from 0 to 65535.";
constexpr std::string_view kPort80Error =
    "Port 80 is not supported: browsers leave it out of the Host header. Use another port.";
constexpr std::string_view kLaunchFileError = "TBR_LAUNCH_FILE must be an absolute path.";
constexpr std::string_view kHostError =
    "TBR_SERVER_HOST must be an IPv4 address or localhost: the link always uses 127.0.0.1.";

/// The port in aText, or nothing unless aText is only digits and the number is 0 to 65535.
std::optional<std::uint16_t> parsePort(std::string_view aText) {
    std::uint32_t number = 0;
    const auto [end, error] = std::from_chars(aText.data(), aText.data() + aText.size(), number);

    // For an unsigned type from_chars refuses a sign; it stops at the first other character, so
    // check the end too.
    if (error != std::errc{} || end != aText.data() + aText.size() || number > 65535)
    {
        return std::nullopt;
    }

    return static_cast<std::uint16_t>(number);
}

} // namespace

std::variant<ServerConfig, std::string> readServerConfig(const EnvLookup& aEnv) {
    ServerConfig config;
    config.mHost = "127.0.0.1";
    config.mPort = 0;

    const std::optional<std::string> host = aEnv("TBR_SERVER_HOST");

    if (host.has_value() && !host->empty())
    {
        // An IPv6 address has colons. The link and the Host check use 127.0.0.1 only, so a
        // server on ::1 could never be reached by its own link.
        if (host->find(':') != std::string::npos)
        {
            return std::string(kHostError);
        }

        config.mHost = *host;
    }

    const std::optional<std::string> port = aEnv("TBR_SERVER_PORT");

    if (port.has_value())
    {
        const std::optional<std::uint16_t> parsed = port->empty() ? std::nullopt : parsePort(*port);

        if (!parsed.has_value())
        {
            return std::string(kPortError);
        }

        if (*parsed == 80)
        {
            return std::string(kPort80Error);
        }

        config.mPort = *parsed;
    }

    const std::optional<std::string> launchFile = aEnv("TBR_LAUNCH_FILE");

    if (launchFile.has_value())
    {
        const std::filesystem::path path(*launchFile);

        if (!path.is_absolute())
        {
            return std::string(kLaunchFileError);
        }

        config.mLaunchFile = path;
    }

    return config;
}

bool isLoopbackHost(std::string_view aHost) {
    return aHost == "127.0.0.1" || aHost == "localhost";
}

} // namespace taxbroker
