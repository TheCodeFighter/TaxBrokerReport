#include "server/routes.hpp"

#include <gtest/gtest.h>

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
          mRoutes(8080, kLink, kSession, mLaunchFile, [this]() { return mNow; }) {}

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
    Routes second(8080, kLink, kSession, mLaunchFile, [this]() { return mNow; });

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

} // namespace
