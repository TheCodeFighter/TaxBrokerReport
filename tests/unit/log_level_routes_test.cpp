#include "../support/log_capture.hpp"
#include "taxbroker/logging.hpp"
#include "server/log_level_routes.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {
using namespace taxbroker;

constexpr std::string_view kPath = "/api/log-level";

HttpResponse get(std::string_view aPath = kPath) {
    return handleLogLevelRoute(HttpRequest{.mMethod = "GET", .mTarget = std::string{aPath}}, aPath);
}

HttpResponse post(const std::string& aBody) {
    return handleLogLevelRoute(
        HttpRequest{.mMethod = "POST", .mTarget = std::string{kPath}, .mBody = aBody},
        kPath);
}

TEST(LogLevelRoutesTest, GetAnswersTheCurrentLevel) {
    LogCapture log;
    setLogLevel(spdlog::level::err);

    const HttpResponse response = get();

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(response.mBody, R"({"level":"error"})");
}

TEST(LogLevelRoutesTest, PostSetsTheLevelAndLogsItOnce) {
    LogCapture log;
    setLogLevel(spdlog::level::err);

    const HttpResponse response = post(R"({"level":"debug"})");

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(response.mBody, R"({"level":"debug"})");
    EXPECT_EQ(currentLogLevel(), spdlog::level::debug);
    EXPECT_EQ(log.count("Log level set: debug"), 1U);
    EXPECT_EQ(get().mBody, R"({"level":"debug"})");
}

TEST(LogLevelRoutesTest, ASetToErrorIsNotShownInTheLogItself) {
    LogCapture log;
    setLogLevel(spdlog::level::info);

    const HttpResponse response = post(R"({"level":"error"})");

    EXPECT_EQ(response.mBody, R"({"level":"error"})");
    EXPECT_EQ(log.count("Log level set"), 0U); // an info line, hidden by the new level
}

TEST(LogLevelRoutesTest, AWrongBodyIsRefusedAndTheLevelStays) {
    LogCapture log;
    setLogLevel(spdlog::level::warn);

    // A level that is not one of the four, a number, no key, an extra key, a body that is not
    // JSON and an empty body.
    for (const std::string body : {R"({"level":"loud"})",
                                   R"({"level":3})",
                                   R"({})",
                                   R"({"level":"info","x":1})",
                                   "not json",
                                   ""})
    {
        const HttpResponse response = post(body);

        EXPECT_EQ(response.mStatus, 400U) << body;
        EXPECT_EQ(response.mBody, R"({"error":"bad_log_level"})") << body;
        EXPECT_EQ(currentLogLevel(), spdlog::level::warn) << body;
    }

    EXPECT_EQ(log.count("Log level request refused: 400 bad_log_level"), 6U);
}

TEST(LogLevelRoutesTest, ABodyOverOneKiBIsRefused) {
    LogCapture log;
    setLogLevel(spdlog::level::warn); // the refusal is a warning

    const HttpResponse response = post(std::string(1025, ' '));

    EXPECT_EQ(response.mStatus, 413U);
    EXPECT_EQ(currentLogLevel(), spdlog::level::warn);
    EXPECT_EQ(log.count("Log level request refused: 413 request_too_large (limit 1024)"), 1U);
}

TEST(LogLevelRoutesTest, AnotherMethodIsRefused) {
    LogCapture log;

    const HttpResponse response =
        handleLogLevelRoute(HttpRequest{.mMethod = "PUT", .mTarget = std::string{kPath}}, kPath);

    EXPECT_EQ(response.mStatus, 405U);
    EXPECT_EQ(log.count("Log level request refused: 405 method_not_allowed"), 1U);
}

TEST(LogLevelRoutesTest, AnotherPathIsNotFound) {
    LogCapture log;

    const HttpResponse response = get("/api/log-level/extra");

    EXPECT_EQ(response.mStatus, 404U);
    EXPECT_EQ(log.count("Log level request refused: 404 not_found"), 1U);
}

} // namespace
