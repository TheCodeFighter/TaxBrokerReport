#include "server/server_config.hpp"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

/// An environment made of a map, so the test names every variable it sets.
EnvLookup envOf(std::map<std::string, std::string> aValues) {
    return [aValues](std::string_view aName) -> std::optional<std::string> {
        const auto found = aValues.find(std::string(aName));

        if (found == aValues.end())
        {
            return std::nullopt;
        }

        return found->second;
    };
}

TEST(ServerConfigTest, EmptyEnvironmentGivesLoopbackAndARandomPort) {
    const auto result = readServerConfig(envOf({}));

    ASSERT_TRUE(std::holds_alternative<ServerConfig>(result));

    const ServerConfig& config = std::get<ServerConfig>(result);
    EXPECT_EQ(config.mHost, "127.0.0.1");
    EXPECT_EQ(config.mPort, 0);
    EXPECT_FALSE(config.mLaunchFile.has_value());
}

TEST(ServerConfigTest, PortIsReadFromTheEnvironment) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_PORT", "8080"}}));

    ASSERT_TRUE(std::holds_alternative<ServerConfig>(result));
    EXPECT_EQ(std::get<ServerConfig>(result).mPort, 8080);
}

TEST(ServerConfigTest, PortZeroIsAllowed) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_PORT", "0"}}));

    ASSERT_TRUE(std::holds_alternative<ServerConfig>(result));
    EXPECT_EQ(std::get<ServerConfig>(result).mPort, 0);
}

TEST(ServerConfigTest, HighestPortIsAllowed) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_PORT", "65535"}}));

    ASSERT_TRUE(std::holds_alternative<ServerConfig>(result));
    EXPECT_EQ(std::get<ServerConfig>(result).mPort, 65535);
}

TEST(ServerConfigTest, PortEightyIsRefused) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_PORT", "80"}}));

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result),
              "Port 80 is not supported: browsers leave it out of the Host header. Use another "
              "port.");
}

TEST(ServerConfigTest, PortAboveTheRangeIsRefused) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_PORT", "65536"}}));

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result),
              "TBR_SERVER_PORT must be a whole number from 0 to 65535.");
}

TEST(ServerConfigTest, NegativePortIsRefused) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_PORT", "-1"}}));

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result),
              "TBR_SERVER_PORT must be a whole number from 0 to 65535.");
}

TEST(ServerConfigTest, PortWithATrailingLetterIsRefused) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_PORT", "8080x"}}));

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result),
              "TBR_SERVER_PORT must be a whole number from 0 to 65535.");
}

TEST(ServerConfigTest, EmptyPortIsRefused) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_PORT", ""}}));

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result),
              "TBR_SERVER_PORT must be a whole number from 0 to 65535.");
}

TEST(ServerConfigTest, PortWithALeadingSpaceIsRefused) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_PORT", " 8080"}}));

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result),
              "TBR_SERVER_PORT must be a whole number from 0 to 65535.");
}

TEST(ServerConfigTest, PortWithAPlusSignIsRefused) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_PORT", "+8080"}}));

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result),
              "TBR_SERVER_PORT must be a whole number from 0 to 65535.");
}

TEST(ServerConfigTest, HostIsReadFromTheEnvironment) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_HOST", "0.0.0.0"}}));

    ASSERT_TRUE(std::holds_alternative<ServerConfig>(result));
    EXPECT_EQ(std::get<ServerConfig>(result).mHost, "0.0.0.0");
}

TEST(ServerConfigTest, EmptyHostKeepsTheLoopback) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_HOST", ""}}));

    ASSERT_TRUE(std::holds_alternative<ServerConfig>(result));
    EXPECT_EQ(std::get<ServerConfig>(result).mHost, "127.0.0.1");
}

TEST(ServerConfigTest, Ipv6LoopbackHostIsRefused) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_HOST", "::1"}}));

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result),
              "TBR_SERVER_HOST must be an IPv4 address or localhost: the link always uses "
              "127.0.0.1.");
}

TEST(ServerConfigTest, Ipv6AnyAddressHostIsRefused) {
    const auto result = readServerConfig(envOf({{"TBR_SERVER_HOST", "::"}}));

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result),
              "TBR_SERVER_HOST must be an IPv4 address or localhost: the link always uses "
              "127.0.0.1.");
}

TEST(ServerConfigTest, LoopbackNamesAreLoopback) {
    EXPECT_TRUE(isLoopbackHost("127.0.0.1"));
    EXPECT_TRUE(isLoopbackHost("localhost"));
}

TEST(ServerConfigTest, OtherAddressesAreNotLoopback) {
    EXPECT_FALSE(isLoopbackHost("0.0.0.0"));
    EXPECT_FALSE(isLoopbackHost("::1"));       // refused earlier by readServerConfig
    EXPECT_FALSE(isLoopbackHost("127.0.0.2")); // only the exact names count
    EXPECT_FALSE(isLoopbackHost("LOCALHOST"));
}

TEST(ServerConfigTest, LaunchFileIsReadFromTheEnvironment) {
    const auto result =
        readServerConfig(envOf({{"TBR_LAUNCH_FILE", "/workspace/runtime/launch/launch.html"}}));

    ASSERT_TRUE(std::holds_alternative<ServerConfig>(result));

    const ServerConfig& config = std::get<ServerConfig>(result);
    ASSERT_TRUE(config.mLaunchFile.has_value());
    EXPECT_EQ(config.mLaunchFile->string(), "/workspace/runtime/launch/launch.html");
}

TEST(ServerConfigTest, RelativeLaunchFileIsRefused) {
    const auto result = readServerConfig(envOf({{"TBR_LAUNCH_FILE", "runtime/launch.html"}}));

    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result), "TBR_LAUNCH_FILE must be an absolute path.");
}

TEST(ServerConfigTest, VersionRequestIsTheOptionAloneAndNothingElse) {
    const std::vector<std::string> noArguments{};
    const std::vector<std::string> versionOnly{"--version"};
    const std::vector<std::string> versionAndMore{"--version", "x"};
    const std::vector<std::string> otherOption{"--help"};

    EXPECT_FALSE(isVersionRequest(noArguments));
    EXPECT_TRUE(isVersionRequest(versionOnly));
    EXPECT_FALSE(isVersionRequest(versionAndMore));
    EXPECT_FALSE(isVersionRequest(otherOption));
}

TEST(ServerConfigTest, SystemEnvironmentFindsASetVariableAndNotAMissingOne) {
    const EnvLookup environment = systemEnvironment();

    EXPECT_TRUE(environment("PATH").has_value());
    EXPECT_FALSE(environment("TBR_THIS_VARIABLE_IS_NOT_SET").has_value());
}

} // namespace
