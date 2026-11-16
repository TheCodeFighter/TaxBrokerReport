#include "../support/log_capture.hpp"
#include "server/routes.hpp"
#include "server/static_files.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {
using namespace taxbroker;

// The link token and the session token of every test: 64 hex characters each, and different.
const std::string kLink = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string kSession = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
const std::string kZeroToken(64, '0');

// Headers the tests use by name, so every request shows its headers.
const HttpField kHost{"Host", "127.0.0.1:8080"};
const HttpField kOrigin{"Origin", "http://127.0.0.1:8080"};
const HttpField kClient{"X-TaxBroker", "1"};
const HttpField kCookie{"Cookie", "tbr_session_8080=" + kSession};

/// One header of a response by name, or nothing.
std::optional<std::string> headerOf(const HttpResponse& aResponse, const std::string& aName) {
    const auto found =
        std::find_if(aResponse.mHeaders.begin(),
                     aResponse.mHeaders.end(),
                     [&aName](const HttpField& aField) { return aField.mName == aName; });

    if (found == aResponse.mHeaders.end())
    {
        return std::nullopt;
    }

    return found->mValue;
}

/// True when any response header name starts with the prefix.
bool hasHeaderStartingWith(const HttpResponse& aResponse, const std::string& aPrefix) {
    return std::any_of(
        aResponse.mHeaders.begin(),
        aResponse.mHeaders.end(),
        [&aPrefix](const HttpField& aField) { return aField.mName.starts_with(aPrefix); });
}

/// The seven security headers with the literals of R18.
void expectSecurityHeaders(const HttpResponse& aResponse) {
    EXPECT_EQ(headerOf(aResponse, "Content-Security-Policy"),
              "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
              "connect-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; "
              "frame-ancestors 'none'");
    EXPECT_EQ(headerOf(aResponse, "X-Content-Type-Options"), "nosniff");
    EXPECT_EQ(headerOf(aResponse, "Referrer-Policy"), "no-referrer");
    EXPECT_EQ(headerOf(aResponse, "X-Frame-Options"), "DENY");
    EXPECT_EQ(headerOf(aResponse, "Cross-Origin-Opener-Policy"), "same-origin");
    EXPECT_EQ(headerOf(aResponse, "Cross-Origin-Resource-Policy"), "same-origin");
    EXPECT_EQ(headerOf(aResponse, "Cache-Control"), "no-store");
}

/// Routes for port 8080 with a fake clock and a launch file in a temp folder.
class ServerRoutesTest : public ::testing::Test {
  protected:
    ServerRoutesTest()
        : mFolder(std::filesystem::temp_directory_path() /
                  (std::string("tbr_routes_") +
                   ::testing::UnitTest::GetInstance()->current_test_info()->name())),
          mLaunchFile(mFolder / "launch.html"),
          mStart(std::chrono::steady_clock::time_point{} + std::chrono::hours(1)), mNow(mStart),
          mRoutes(
              8080,
              kLink,
              kSession,
              mLaunchFile,
              [this]() { return mNow; },
              1000,
              placeholderStaticFiles(),
              "9.8.7") {}

    void SetUp() override {
        std::filesystem::remove_all(mFolder);
        std::filesystem::create_directories(mFolder);
        std::ofstream(mLaunchFile) << "launch page";
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(mFolder, ignored);
    }

    std::filesystem::path mFolder;
    std::filesystem::path mLaunchFile;
    std::chrono::steady_clock::time_point mStart;
    std::chrono::steady_clock::time_point mNow;
    Routes mRoutes;
};

std::string loginBody(const std::string& aToken) {
    return "{\"token\":\"" + aToken + "\"}";
}

// ---- Login (R4, R5, R6, R19)

TEST_F(ServerRoutesTest, RightLinkTokenGivesTheSessionCookieAndDeletesTheLaunchFile) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kOrigin, kClient},
                              .mBody = loginBody(kLink)};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 204U);
    EXPECT_EQ(headerOf(response, "Set-Cookie"),
              "tbr_session_8080=" + kSession + "; HttpOnly; SameSite=Strict; Path=/");
    EXPECT_FALSE(std::filesystem::exists(mLaunchFile));
    EXPECT_EQ(response.mBody, "");
}

TEST_F(ServerRoutesTest, LinkTokenWorksOnlyOnce) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kOrigin, kClient},
                              .mBody = loginBody(kLink)};
    EXPECT_EQ(mRoutes.handle(request).mStatus, 204U);

    const HttpResponse second = mRoutes.handle(request);

    EXPECT_EQ(second.mStatus, 401U);
    EXPECT_FALSE(headerOf(second, "Set-Cookie").has_value());
}

TEST_F(ServerRoutesTest, WrongLinkTokenDoesNotUseUpTheRightOne) {
    const HttpRequest wrong{.mMethod = "POST",
                            .mTarget = "/api/session",
                            .mHeaders = {kHost, kOrigin, kClient},
                            .mBody = loginBody(kZeroToken)};
    const HttpRequest right{.mMethod = "POST",
                            .mTarget = "/api/session",
                            .mHeaders = {kHost, kOrigin, kClient},
                            .mBody = loginBody(kLink)};

    EXPECT_EQ(mRoutes.handle(wrong).mStatus, 401U);
    EXPECT_TRUE(std::filesystem::exists(mLaunchFile));

    EXPECT_EQ(mRoutes.handle(right).mStatus, 204U);
}

TEST_F(ServerRoutesTest, LinkTokenWorksAt119SecondsAndNotAt120Seconds) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kOrigin, kClient},
                              .mBody = loginBody(kLink)};
    Routes second(
        8080,
        kLink,
        kSession,
        mLaunchFile,
        [this]() { return mNow; },
        1000,
        placeholderStaticFiles());

    mNow = mStart + std::chrono::seconds(119);
    EXPECT_EQ(mRoutes.handle(request).mStatus, 204U);

    mNow = mStart + std::chrono::seconds(120);
    EXPECT_EQ(second.handle(request).mStatus, 401U);
}

TEST_F(ServerRoutesTest, LoginBodyThatIsNotJsonGivesBadRequest) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kOrigin, kClient},
                              .mBody = "token=abc"};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"bad_request"})");
    EXPECT_EQ(headerOf(response, "Content-Type"), "application/json");
}

TEST_F(ServerRoutesTest, LoginBodyWithoutATokenGivesBadRequest) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kOrigin, kClient},
                              .mBody = "{}"};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 400U);
}

TEST_F(ServerRoutesTest, LoginBodyWithANumberAsTokenGivesBadRequest) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kOrigin, kClient},
                              .mBody = R"({"token":5})"};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 400U);
}

TEST_F(ServerRoutesTest, LoginWorksWithTheLocalhostNames) {
    const HttpRequest request{
        .mMethod = "POST",
        .mTarget = "/api/session",
        .mHeaders = {{"Host", "localhost:8080"}, {"Origin", "http://localhost:8080"}, kClient},
        .mBody = loginBody(kLink)};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 204U);
}

// ---- Guard (R5, R7, R8, R9, R17)

TEST_F(ServerRoutesTest, ForeignHostIsRejected) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/",
                              .mHeaders = {{"Host", "evil.example:8080"}}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 403U);
    EXPECT_EQ(response.mBody, "");
}

TEST_F(ServerRoutesTest, HostWithAnotherPortIsRejected) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/",
                              .mHeaders = {{"Host", "127.0.0.1:8081"}}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 403U);
}

TEST_F(ServerRoutesTest, MissingHostIsRejected) {
    const HttpRequest request{.mMethod = "GET", .mTarget = "/", .mHeaders = {}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 403U);
}

TEST_F(ServerRoutesTest, TwoHostHeadersAreRejected) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/",
                              .mHeaders = {{"Host", "127.0.0.1:8080"}, {"Host", "127.0.0.1:8080"}}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 403U);
}

TEST_F(ServerRoutesTest, HostHeaderNameIsFoundWithoutCase) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/",
                              .mHeaders = {{"hOsT", "127.0.0.1:8080"}}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 200U);
}

TEST_F(ServerRoutesTest, LoginWithoutOriginIsRejected) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kClient},
                              .mBody = loginBody(kLink)};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 403U);
    EXPECT_TRUE(std::filesystem::exists(mLaunchFile));
}

TEST_F(ServerRoutesTest, LoginWithForeignOriginIsRejected) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, {"Origin", "http://evil.example"}, kClient},
                              .mBody = loginBody(kLink)};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 403U);
}

TEST_F(ServerRoutesTest, LoginWithNullOriginIsRejected) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, {"Origin", "null"}, kClient},
                              .mBody = loginBody(kLink)};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 403U);
}

TEST_F(ServerRoutesTest, LoginWithHttpsOriginIsRejected) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, {"Origin", "https://127.0.0.1:8080"}, kClient},
                              .mBody = loginBody(kLink)};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 403U);
}

TEST_F(ServerRoutesTest, TwoOriginHeadersAreRejected) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kOrigin, kOrigin, kClient},
                              .mBody = loginBody(kLink)};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 403U);
}

TEST_F(ServerRoutesTest, SafeRequestMayOmitTheOrigin) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kClient, kCookie}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 204U);
}

TEST_F(ServerRoutesTest, ApiRequestWithoutTheClientHeaderIsRejected) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kCookie}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 403U);
}

TEST_F(ServerRoutesTest, ApiRequestWithClientHeaderZeroIsRejected) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, {"X-TaxBroker", "0"}, kCookie}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 403U);
}

TEST_F(ServerRoutesTest, ApiRequestWithTheClientHeaderTwiceIsRejected) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kClient, kClient, kCookie}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 403U);
}

TEST_F(ServerRoutesTest, SessionCheckWithoutACookieIsUnauthorized) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kClient}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 401U);
    EXPECT_EQ(response.mBody, "");
}

TEST_F(ServerRoutesTest, SessionCheckWithAWrongTokenIsUnauthorized) {
    const HttpRequest request{
        .mMethod = "GET",
        .mTarget = "/api/session",
        .mHeaders = {kHost, kClient, {"Cookie", "tbr_session_8080=" + kZeroToken}}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 401U);
}

TEST_F(ServerRoutesTest, SessionCookieOfAnotherPortIsUnauthorized) {
    const HttpRequest request{
        .mMethod = "GET",
        .mTarget = "/api/session",
        .mHeaders = {kHost, kClient, {"Cookie", "tbr_session_8081=" + kSession}}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 401U);
}

TEST_F(ServerRoutesTest, SessionCookieTwiceIsUnauthorized) {
    const HttpRequest request{
        .mMethod = "GET",
        .mTarget = "/api/session",
        .mHeaders = {
            kHost,
            kClient,
            {"Cookie", "tbr_session_8080=" + kSession + "; tbr_session_8080=" + kSession}}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 401U);
}

TEST_F(ServerRoutesTest, LinkTokenAsSessionCookieIsUnauthorized) {
    const HttpRequest request{
        .mMethod = "GET",
        .mTarget = "/api/session",
        .mHeaders = {kHost, kClient, {"Cookie", "tbr_session_8080=" + kLink}}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 401U);
}

TEST_F(ServerRoutesTest, SessionCookieAmongOtherCookiesIsAccepted) {
    const HttpRequest request{
        .mMethod = "GET",
        .mTarget = "/api/session",
        .mHeaders = {kHost, kClient, {"Cookie", "theme=dark; tbr_session_8080=" + kSession}}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 204U);
}

TEST_F(ServerRoutesTest, SessionCookieInASecondCookieHeaderIsAccepted) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kClient, {"Cookie", "theme=dark"}, kCookie}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 204U);
}

TEST_F(ServerRoutesTest, UnknownApiPathWithoutACookieIsUnauthorizedNotNotFound) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/nothing",
                              .mHeaders = {kHost, kClient}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 401U);
}

TEST_F(ServerRoutesTest, UnknownApiPathWithTheCookieIsNotFound) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/nothing",
                              .mHeaders = {kHost, kClient, kCookie}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 404U);
}

TEST_F(ServerRoutesTest, PreflightGetsNoCorsHeaders) {
    const HttpRequest request{
        .mMethod = "OPTIONS",
        .mTarget = "/api/session",
        .mHeaders = {kHost, kOrigin, kClient, kCookie, {"Access-Control-Request-Method", "POST"}}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 405U);
    EXPECT_EQ(headerOf(response, "Allow"), "GET, POST");
    EXPECT_FALSE(hasHeaderStartingWith(response, "Access-Control-"));
}

TEST_F(ServerRoutesTest, ForeignPreflightIsRejectedWithoutCorsHeaders) {
    const HttpRequest request{.mMethod = "OPTIONS",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost,
                                           {"Origin", "http://evil.example"},
                                           {"Access-Control-Request-Method", "POST"}}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 403U);
    EXPECT_FALSE(hasHeaderStartingWith(response, "Access-Control-"));
}

// ---- Routes and headers (R18)

TEST_F(ServerRoutesTest, RootPageIsThePlaceholder) {
    const HttpRequest request{.mMethod = "GET", .mTarget = "/", .mHeaders = {kHost}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(headerOf(response, "Content-Type"), "text/html; charset=utf-8");
    EXPECT_NE(response.mBody.find("TaxBrokerReport is running"), std::string::npos);
    EXPECT_EQ(response.mBody.find("<script"), std::string::npos);
}

TEST_F(ServerRoutesTest, MessagesNeedTheSession) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/messages",
                              .mHeaders = {kHost, kOrigin, kClient}};
    LogCapture log;

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 401U);
    EXPECT_EQ(log.count("Request refused: 401 no_session"), 1U);
}

TEST_F(ServerRoutesTest, MessagesWithTheSessionAreJson) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/messages",
                              .mHeaders = {kHost, kOrigin, kClient, kCookie}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(headerOf(response, "Content-Type"), "application/json");
    expectSecurityHeaders(response);

    const nlohmann::json body = nlohmann::json::parse(response.mBody);

    EXPECT_TRUE(body.at("messages").contains("parse_error.unclosed_quote"));
    EXPECT_EQ(body.at("terms").size(), 9U);
}

TEST_F(ServerRoutesTest, PostToMessagesIsNotAllowed) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/messages",
                              .mHeaders = {kHost, kOrigin, kClient, kCookie}};
    LogCapture log;

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 405U);
    EXPECT_EQ(headerOf(response, "Allow"), "GET");
    EXPECT_EQ(log.count("Request refused: 405 method_not_allowed"), 1U);
}

TEST_F(ServerRoutesTest, VersionNeedsTheSession) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/version",
                              .mHeaders = {kHost, kOrigin, kClient}};
    LogCapture log;

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 401U);
    EXPECT_EQ(log.count("Request refused: 401 no_session"), 1U);
}

TEST_F(ServerRoutesTest, VersionWithTheSessionIsJson) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/version",
                              .mHeaders = {kHost, kOrigin, kClient, kCookie}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(headerOf(response, "Content-Type"), "application/json");
    expectSecurityHeaders(response);

    // The version the routes were made with, not the project version, so a release needs no
    // test change.
    EXPECT_EQ(response.mBody, R"({"version":"9.8.7"})");
}

TEST_F(ServerRoutesTest, PostToVersionIsNotAllowed) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/version",
                              .mHeaders = {kHost, kOrigin, kClient, kCookie}};
    LogCapture log;

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 405U);
    EXPECT_EQ(headerOf(response, "Allow"), "GET");
    EXPECT_EQ(log.count("Request refused: 405 method_not_allowed"), 1U);
}

TEST_F(ServerRoutesTest, HeadRootHasNoBody) {
    const HttpRequest request{.mMethod = "HEAD", .mTarget = "/", .mHeaders = {kHost}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(response.mBody, "");
}

TEST_F(ServerRoutesTest, PostToRootIsNotAllowed) {
    const HttpRequest request{.mMethod = "POST", .mTarget = "/", .mHeaders = {kHost, kOrigin}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 405U);
    EXPECT_EQ(headerOf(response, "Allow"), "GET, HEAD");
}

TEST_F(ServerRoutesTest, UnknownPageIsNotFound) {
    const HttpRequest request{.mMethod = "GET", .mTarget = "/missing", .mHeaders = {kHost}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 404U);
}

TEST_F(ServerRoutesTest, SessionPathWithATrailingSlashIsNotFound) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/session/",
                              .mHeaders = {kHost, kClient, kCookie}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 404U);
}

TEST_F(ServerRoutesTest, QueryStringIsCutOffBeforeRouting) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/session?x=1",
                              .mHeaders = {kHost, kClient, kCookie}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 204U);
}

TEST_F(ServerRoutesTest, AbsoluteFormTargetIsABadRequest) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "http://127.0.0.1:8080/",
                              .mHeaders = {kHost}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 400U);
}

TEST_F(ServerRoutesTest, EmptyTargetIsABadRequest) {
    const HttpRequest request{.mMethod = "GET", .mTarget = "", .mHeaders = {kHost}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 400U);
}

TEST_F(ServerRoutesTest, ApiRootPathNeedsTheSessionToo) {
    const HttpRequest request{.mMethod = "GET", .mTarget = "/api", .mHeaders = {kHost, kClient}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 401U);
}

TEST_F(ServerRoutesTest, SecurityHeadersOn200) {
    const HttpRequest request{.mMethod = "GET", .mTarget = "/", .mHeaders = {kHost}};

    expectSecurityHeaders(mRoutes.handle(request));
}

TEST_F(ServerRoutesTest, SecurityHeadersOn204) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kClient, kCookie}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 204U);
    expectSecurityHeaders(response);
}

TEST_F(ServerRoutesTest, SecurityHeadersOn400) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kOrigin, kClient},
                              .mBody = "{}"};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 400U);
    expectSecurityHeaders(response);
}

TEST_F(ServerRoutesTest, SecurityHeadersOn401) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kClient}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 401U);
    expectSecurityHeaders(response);
}

TEST_F(ServerRoutesTest, SecurityHeadersOn403) {
    const HttpRequest request{.mMethod = "GET", .mTarget = "/", .mHeaders = {}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 403U);
    expectSecurityHeaders(response);
}

TEST_F(ServerRoutesTest, SecurityHeadersOn404) {
    const HttpRequest request{.mMethod = "GET", .mTarget = "/missing", .mHeaders = {kHost}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 404U);
    expectSecurityHeaders(response);
}

TEST_F(ServerRoutesTest, SecurityHeadersOn405) {
    const HttpRequest request{.mMethod = "POST", .mTarget = "/", .mHeaders = {kHost, kOrigin}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 405U);
    expectSecurityHeaders(response);
}

TEST(ServerRoutesErrorTest, ErrorResponseHasTheStatusTheHeadersAndNoBody) {
    const HttpResponse response = makeErrorResponse(431);

    EXPECT_EQ(response.mStatus, 431U);
    EXPECT_EQ(response.mBody, "");
    expectSecurityHeaders(response);
}

// ---- Log lines of the refusals (R13)

TEST_F(ServerRoutesTest, ARefusedHostLogsOneWarningEvenWhenTheBodyLimitIsAskedFirst) {
    LogCapture log;
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/session",
                              .mHeaders = {{"Host", "evil.example:8080"}, kClient, kCookie}};

    // The server asks for the body limit before it handles the request: one line, not two.
    (void)mRoutes.bodyPolicy(request);
    EXPECT_EQ(mRoutes.handle(request).mStatus, 403U);

    EXPECT_EQ(log.count("Request refused: 403 host_not_allowed"), 1U);
    EXPECT_EQ(log.count("Request refused"), 1U);
    EXPECT_EQ(log.text().find("evil.example"), std::string::npos);
}

TEST_F(ServerRoutesTest, ARefusedTargetOriginAndClientHeaderEachLogTheirReason) {
    LogCapture log;
    const HttpRequest absoluteTarget{.mMethod = "GET",
                                     .mTarget = "http://127.0.0.1:8080/api/session",
                                     .mHeaders = {kHost}};
    const HttpRequest twoOrigins{.mMethod = "GET",
                                 .mTarget = "/api/session",
                                 .mHeaders = {kHost, kOrigin, kOrigin, kClient, kCookie}};
    const HttpRequest noOrigin{.mMethod = "POST",
                               .mTarget = "/api/session",
                               .mHeaders = {kHost, kClient},
                               .mBody = loginBody(kLink)};
    const HttpRequest foreignOrigin{.mMethod = "POST",
                                    .mTarget = "/api/session",
                                    .mHeaders = {kHost, {"Origin", "http://evil.example"}, kClient},
                                    .mBody = loginBody(kLink)};
    const HttpRequest noClient{.mMethod = "GET",
                               .mTarget = "/api/session",
                               .mHeaders = {kHost, kCookie}};

    EXPECT_EQ(mRoutes.handle(absoluteTarget).mStatus, 400U);
    EXPECT_EQ(mRoutes.handle(twoOrigins).mStatus, 403U);
    EXPECT_EQ(mRoutes.handle(noOrigin).mStatus, 403U);
    EXPECT_EQ(mRoutes.handle(foreignOrigin).mStatus, 403U);
    EXPECT_EQ(mRoutes.handle(noClient).mStatus, 403U);

    EXPECT_EQ(log.count("Request refused: 400 bad_target"), 1U);
    EXPECT_EQ(log.count("Request refused: 403 origin_repeated"), 1U);
    EXPECT_EQ(log.count("Request refused: 403 origin_missing"), 1U);
    EXPECT_EQ(log.count("Request refused: 403 origin_not_allowed"), 1U);
    EXPECT_EQ(log.count("Request refused: 403 client_header"), 1U);
    EXPECT_EQ(log.count("Request refused"), 5U);
    EXPECT_EQ(log.text().find("evil.example"), std::string::npos);
}

TEST_F(ServerRoutesTest, AMissingAndAWrongSessionLogDifferentReasonsWithoutTheToken) {
    LogCapture log;
    const HttpRequest noCookie{.mMethod = "GET",
                               .mTarget = "/api/session",
                               .mHeaders = {kHost, kClient}};
    const HttpRequest wrongToken{
        .mMethod = "GET",
        .mTarget = "/api/session",
        .mHeaders = {kHost, kClient, {"Cookie", "tbr_session_8080=" + kZeroToken}}};

    EXPECT_EQ(mRoutes.handle(noCookie).mStatus, 401U);
    EXPECT_EQ(mRoutes.handle(wrongToken).mStatus, 401U);

    EXPECT_EQ(log.count("Request refused: 401 no_session"), 1U);
    EXPECT_EQ(log.count("Request refused: 401 session_wrong"), 1U);
    EXPECT_EQ(log.text().find(kZeroToken), std::string::npos);
    EXPECT_EQ(log.text().find(kSession), std::string::npos);
}

TEST_F(ServerRoutesTest, UnknownApiPathAndWrongSessionMethodLogTheirStatus) {
    LogCapture log;
    const HttpRequest unknownPath{.mMethod = "GET",
                                  .mTarget = "/api/nothing",
                                  .mHeaders = {kHost, kClient, kCookie}};
    const HttpRequest putSession{.mMethod = "PUT",
                                 .mTarget = "/api/session",
                                 .mHeaders = {kHost, kOrigin, kClient, kCookie}};

    EXPECT_EQ(mRoutes.handle(unknownPath).mStatus, 404U);
    EXPECT_EQ(mRoutes.handle(putSession).mStatus, 405U);

    EXPECT_EQ(log.count("Request refused: 404 not_found"), 1U);
    EXPECT_EQ(log.count("Request refused: 405 method_not_allowed"), 1U);
}

TEST_F(ServerRoutesTest, APageRequestWritesNoRefusalLine) {
    LogCapture log;
    const HttpRequest page{.mMethod = "GET", .mTarget = "/favicon.ico", .mHeaders = {kHost}};

    EXPECT_EQ(mRoutes.handle(page).mStatus, 404U);

    EXPECT_EQ(log.count("refused"), 0U);
}

TEST_F(ServerRoutesTest, LoginRefusalsLogTheLinkProblemWithoutTheToken) {
    const HttpRequest right{.mMethod = "POST",
                            .mTarget = "/api/session",
                            .mHeaders = {kHost, kOrigin, kClient},
                            .mBody = loginBody(kLink)};
    const HttpRequest wrong{.mMethod = "POST",
                            .mTarget = "/api/session",
                            .mHeaders = {kHost, kOrigin, kClient},
                            .mBody = loginBody(kZeroToken)};
    const HttpRequest noJson{.mMethod = "POST",
                             .mTarget = "/api/session",
                             .mHeaders = {kHost, kOrigin, kClient},
                             .mBody = "token=abc"};
    LogCapture log;

    EXPECT_EQ(mRoutes.handle(noJson).mStatus, 400U);
    EXPECT_EQ(mRoutes.handle(wrong).mStatus, 401U);
    EXPECT_EQ(mRoutes.handle(right).mStatus, 204U);
    EXPECT_EQ(mRoutes.handle(right).mStatus, 401U);

    EXPECT_EQ(log.count("Login refused: 400 bad_request"), 1U);
    EXPECT_EQ(log.count("Login refused: 401 link_wrong"), 1U);
    EXPECT_EQ(log.count("Browser login done"), 1U);
    EXPECT_EQ(log.count("Login refused: 401 link_used"), 1U);
    EXPECT_EQ(log.count("Login refused"), 3U);
    EXPECT_EQ(log.text().find(kLink), std::string::npos);
    EXPECT_EQ(log.text().find(kSession), std::string::npos);
}

TEST_F(ServerRoutesTest, AnExpiredLinkLogsLinkExpired) {
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/api/session",
                              .mHeaders = {kHost, kOrigin, kClient},
                              .mBody = loginBody(kLink)};
    LogCapture log;

    mNow = mStart + std::chrono::seconds(120);

    EXPECT_EQ(mRoutes.handle(request).mStatus, 401U);

    EXPECT_EQ(log.count("Login refused: 401 link_expired"), 1U);
}

// ---- Secrets (R4)

TEST_F(ServerRoutesTest, NoBodyHoldsTheLinkTokenOrTheSessionToken) {
    const HttpRequest wrong{.mMethod = "POST",
                            .mTarget = "/api/session",
                            .mHeaders = {kHost, kOrigin, kClient},
                            .mBody = loginBody(kZeroToken)};
    const HttpRequest right{.mMethod = "POST",
                            .mTarget = "/api/session",
                            .mHeaders = {kHost, kOrigin, kClient},
                            .mBody = loginBody(kLink)};
    const HttpRequest page{.mMethod = "GET", .mTarget = "/", .mHeaders = {kHost}};
    const HttpRequest check{.mMethod = "GET",
                            .mTarget = "/api/session",
                            .mHeaders = {kHost, kClient, kCookie}};

    const std::vector<HttpResponse> responses = {mRoutes.handle(wrong),
                                                 mRoutes.handle(right),
                                                 mRoutes.handle(page),
                                                 mRoutes.handle(check)};

    for (const HttpResponse& response : responses)
    {
        EXPECT_EQ(response.mBody.find(kLink), std::string::npos);
        EXPECT_EQ(response.mBody.find(kSession), std::string::npos);
    }
}

// ---- Body policy and the run routes

TEST_F(ServerRoutesTest, UploadWithASessionMayHaveThirtyThreeFilesOfBody) {
    const HttpRequest header{.mMethod = "POST",
                             .mTarget = "/api/run",
                             .mHeaders = {kHost, kOrigin, kClient, kCookie}};

    const BodyPolicy policy = mRoutes.bodyPolicy(header);

    // The file limit of this fixture is 1000 bytes: the request part and 32 files, 33 x 1000.
    EXPECT_EQ(policy.mLimit, 33000U);
    EXPECT_TRUE(policy.mGuardPassed);
    EXPECT_FALSE(policy.mRefusal.has_value());
}

TEST_F(ServerRoutesTest, UploadWithoutTheCookieGetsTheDefaultLimitAndNoPass) {
    const HttpRequest header{.mMethod = "POST",
                             .mTarget = "/api/run",
                             .mHeaders = {kHost, kOrigin, kClient}};

    const BodyPolicy policy = mRoutes.bodyPolicy(header);

    EXPECT_EQ(policy.mLimit, 1024U * 1024U);
    EXPECT_FALSE(policy.mGuardPassed);

    // The guard's own answer and its word travel with the policy, for the log of the server.
    ASSERT_TRUE(policy.mRefusal.has_value());

    EXPECT_EQ(policy.mRefusal->mStatus, 401U);
    EXPECT_EQ(policy.mReason, "no_session");
}

TEST_F(ServerRoutesTest, UploadWithAWrongSessionCarriesTheWrongSessionRefusal) {
    // A tab that was open before a restart of the server sends the old session token.
    const HttpRequest header{
        .mMethod = "POST",
        .mTarget = "/api/run",
        .mHeaders = {kHost, kOrigin, kClient, {"Cookie", "tbr_session_8080=" + kZeroToken}}};

    const BodyPolicy policy = mRoutes.bodyPolicy(header);

    ASSERT_TRUE(policy.mRefusal.has_value());

    EXPECT_EQ(policy.mRefusal->mStatus, 401U);
    EXPECT_EQ(policy.mReason, "session_wrong");
}

TEST_F(ServerRoutesTest, UploadWithAForeignOriginGetsTheDefaultLimitAndNoPass) {
    const HttpRequest header{
        .mMethod = "POST",
        .mTarget = "/api/run",
        .mHeaders = {kHost, {"Origin", "http://evil.example"}, kClient, kCookie}};

    const BodyPolicy policy = mRoutes.bodyPolicy(header);

    EXPECT_EQ(policy.mLimit, 1024U * 1024U);
    EXPECT_FALSE(policy.mGuardPassed);

    ASSERT_TRUE(policy.mRefusal.has_value());

    EXPECT_EQ(policy.mRefusal->mStatus, 403U);
    EXPECT_EQ(policy.mReason, "origin_not_allowed");
}

TEST_F(ServerRoutesTest, GetResultWithASessionGetsTheDefaultLimitAndAPass) {
    const HttpRequest header{.mMethod = "GET",
                             .mTarget = "/api/run",
                             .mHeaders = {kHost, kClient, kCookie}};

    const BodyPolicy policy = mRoutes.bodyPolicy(header);

    EXPECT_EQ(policy.mLimit, 1024U * 1024U);
    EXPECT_TRUE(policy.mGuardPassed);
}

TEST_F(ServerRoutesTest, AnswerCallWithASessionGetsTheDefaultLimit) {
    const HttpRequest header{.mMethod = "POST",
                             .mTarget = "/api/run/answers",
                             .mHeaders = {kHost, kOrigin, kClient, kCookie}};

    EXPECT_EQ(mRoutes.bodyPolicy(header).mLimit, 1024U * 1024U);
}

TEST_F(ServerRoutesTest, LoginAndPageGetTheDefaultLimitAndAPass) {
    const HttpRequest login{.mMethod = "POST",
                            .mTarget = "/api/session",
                            .mHeaders = {kHost, kOrigin, kClient}};
    const HttpRequest page{.mMethod = "GET", .mTarget = "/", .mHeaders = {kHost}};

    EXPECT_EQ(mRoutes.bodyPolicy(login).mLimit, 1024U * 1024U);
    EXPECT_TRUE(mRoutes.bodyPolicy(login).mGuardPassed);
    EXPECT_EQ(mRoutes.bodyPolicy(page).mLimit, 1024U * 1024U);
    EXPECT_TRUE(mRoutes.bodyPolicy(page).mGuardPassed);
}

TEST_F(ServerRoutesTest, BodyPolicyDoesNotRedeemTheLinkToken) {
    const HttpRequest login{.mMethod = "POST",
                            .mTarget = "/api/session",
                            .mHeaders = {kHost, kOrigin, kClient},
                            .mBody = loginBody(kLink)};

    (void)mRoutes.bodyPolicy(login);

    EXPECT_EQ(mRoutes.handle(login).mStatus, 204U);
}

TEST_F(ServerRoutesTest, DownloadWithoutARunIsNotFound) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/run/reports/capitalGains/xml?revision=x",
                              .mHeaders = {kHost, kClient, kCookie}};

    const HttpResponse response = mRoutes.handle(request);

    EXPECT_EQ(response.mStatus, 404U);
    EXPECT_EQ(response.mBody, R"({"error":"no_run"})");
}

TEST_F(ServerRoutesTest, DownloadWithARevisionThatIsNotANumberIsBadRequest) {
    // An upload with one file that holds only the header: a run with nothing to report.
    const std::string body =
        "--b\r\nContent-Disposition: form-data; name=\"request\"\r\n\r\n"
        R"({"reportingYear":2024,"filingType":"original","taxpayer":{"taxNumber":"12345678","isResident":true},)"
        R"("files":[{"part":"file0","kind":"broker","broker":"trade_republic"}]})"
        "\r\n--b\r\nContent-Disposition: form-data; name=\"file0\"; filename=\"empty.csv\"\r\n\r\n"
        "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,"
        "tax,currency,original_amount,original_currency,fx_rate,description,transaction_id,"
        "counterparty_name,counterparty_iban,payment_reference,mcc_code\n"
        "\r\n--b--\r\n";
    const HttpRequest upload{.mMethod = "POST",
                             .mTarget = "/api/run",
                             .mHeaders = {kHost,
                                          kOrigin,
                                          kClient,
                                          kCookie,
                                          {"Content-Type", "multipart/form-data; boundary=b"}},
                             .mBody = body};

    ASSERT_EQ(mRoutes.handle(upload).mStatus, 200U);

    const HttpRequest download{.mMethod = "GET",
                               .mTarget = "/api/run/reports/capitalGains/xml?revision=x",
                               .mHeaders = {kHost, kClient, kCookie}};

    const HttpResponse response = mRoutes.handle(download);

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"bad_request"})");
}

TEST_F(ServerRoutesTest, OtherMethodsOnRunPathsAreNotAllowed) {
    const HttpRequest put{.mMethod = "PUT",
                          .mTarget = "/api/run",
                          .mHeaders = {kHost, kOrigin, kClient, kCookie}};
    const HttpRequest getAnswers{.mMethod = "GET",
                                 .mTarget = "/api/run/answers",
                                 .mHeaders = {kHost, kClient, kCookie}};
    const HttpRequest getDelete{.mMethod = "GET",
                                .mTarget = "/api/run/decisions/delete",
                                .mHeaders = {kHost, kClient, kCookie}};
    const HttpRequest postDownload{.mMethod = "POST",
                                   .mTarget = "/api/run/reports/capitalGains/xml?revision=1",
                                   .mHeaders = {kHost, kOrigin, kClient, kCookie}};

    EXPECT_EQ(mRoutes.handle(put).mStatus, 405U);
    EXPECT_EQ(headerOf(mRoutes.handle(put), "Allow"), "GET, POST");
    EXPECT_EQ(mRoutes.handle(getAnswers).mStatus, 405U);
    EXPECT_EQ(headerOf(mRoutes.handle(getAnswers), "Allow"), "POST");
    EXPECT_EQ(mRoutes.handle(getDelete).mStatus, 405U);
    EXPECT_EQ(headerOf(mRoutes.handle(getDelete), "Allow"), "POST");
    EXPECT_EQ(mRoutes.handle(postDownload).mStatus, 405U);
    EXPECT_EQ(headerOf(mRoutes.handle(postDownload), "Allow"), "GET");
}

TEST_F(ServerRoutesTest, UnknownPathBelowRunIsNotFound) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/run/nothing",
                              .mHeaders = {kHost, kClient, kCookie}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 404U);
}

TEST_F(ServerRoutesTest, RunPathWithoutTheCookieIsUnauthorized) {
    const HttpRequest request{.mMethod = "GET",
                              .mTarget = "/api/run/nothing",
                              .mHeaders = {kHost, kClient}};

    EXPECT_EQ(mRoutes.handle(request).mStatus, 401U);
}

} // namespace
