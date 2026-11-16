#include "server/routes.hpp"
#include "server/static_files.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace {
using namespace taxbroker;

const std::string kLink = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string kSession = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

const HttpField kHost{"Host", "127.0.0.1:8080"};
const HttpField kOrigin{"Origin", "http://127.0.0.1:8080"};

// A tiny table: the tests do not need the real frontend, and no Node.
const std::array<StaticFile, 4> kFiles{
    StaticFile{"index.html", "<!doctype html><title>t</title>"},
    StaticFile{"assets/app.js", "console.log(1);"},
    StaticFile{"assets/app.css", "body{}"},
    StaticFile{"assets/logo.webp", "RIFF"},
};

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

bool hasHeaderStartingWith(const HttpResponse& aResponse, const std::string& aPrefix) {
    return std::any_of(
        aResponse.mHeaders.begin(),
        aResponse.mHeaders.end(),
        [&aPrefix](const HttpField& aField) { return aField.mName.starts_with(aPrefix); });
}

/// The security headers (R18) of the page and its files, and no CORS header (R9).
void expectSecurityHeadersWithoutCors(const HttpResponse& aResponse) {
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
    EXPECT_FALSE(hasHeaderStartingWith(aResponse, "Access-Control-"));
}

/// Routes for port 8080 that serve the given table. The launch file is never touched.
Routes makeRoutes(std::span<const StaticFile> aFiles) {
    return Routes(
        8080,
        kLink,
        kSession,
        std::filesystem::path("unused_launch.html"),
        []() { return std::chrono::steady_clock::time_point{} + std::chrono::hours(1); },
        1000,
        aFiles,
        "9.8.7",
        ProfileStore(std::filesystem::path("unused") / "profile.json", std::nullopt));
}

HttpResponse get(Routes& aRoutes, const std::string& aTarget) {
    return aRoutes.handle(HttpRequest{.mMethod = "GET", .mTarget = aTarget, .mHeaders = {kHost}});
}

TEST(ServerStaticFilesTest, RootIsTheIndexPage) {
    Routes routes = makeRoutes(kFiles);

    const HttpResponse response = get(routes, "/");

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(headerOf(response, "Content-Type"), "text/html; charset=utf-8");
    EXPECT_EQ(response.mBody, "<!doctype html><title>t</title>");
    expectSecurityHeadersWithoutCors(response);
}

TEST(ServerStaticFilesTest, RootWithAQueryIsTheIndexPage) {
    Routes routes = makeRoutes(kFiles);

    const HttpResponse response = get(routes, "/?x=1");

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(headerOf(response, "Content-Type"), "text/html; charset=utf-8");
    EXPECT_EQ(response.mBody, "<!doctype html><title>t</title>");
    expectSecurityHeadersWithoutCors(response);
}

TEST(ServerStaticFilesTest, ScriptIsServedAsJavaScript) {
    Routes routes = makeRoutes(kFiles);

    const HttpResponse response = get(routes, "/assets/app.js");

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(headerOf(response, "Content-Type"), "text/javascript; charset=utf-8");
    EXPECT_EQ(response.mBody, "console.log(1);");
    expectSecurityHeadersWithoutCors(response);
}

TEST(ServerStaticFilesTest, StyleSheetIsServedAsCss) {
    Routes routes = makeRoutes(kFiles);

    const HttpResponse response = get(routes, "/assets/app.css");

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(headerOf(response, "Content-Type"), "text/css; charset=utf-8");
    EXPECT_EQ(response.mBody, "body{}");
    expectSecurityHeadersWithoutCors(response);
}

TEST(ServerStaticFilesTest, HeadOfTheScriptHasTheTypeAndNoBody) {
    Routes routes = makeRoutes(kFiles);
    const HttpRequest request{.mMethod = "HEAD", .mTarget = "/assets/app.js", .mHeaders = {kHost}};

    const HttpResponse response = routes.handle(request);

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(headerOf(response, "Content-Type"), "text/javascript; charset=utf-8");
    EXPECT_EQ(response.mBody, "");
    expectSecurityHeadersWithoutCors(response);
}

TEST(ServerStaticFilesTest, LogoIsServedAsWebpBytes) {
    Routes routes = makeRoutes(kFiles);

    const HttpResponse response = get(routes, "/assets/logo.webp");

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(headerOf(response, "Content-Type"), "image/webp");
    EXPECT_EQ(response.mBody, "RIFF");
    expectSecurityHeadersWithoutCors(response);
}

TEST(ServerStaticFilesTest, IndexHtmlIsNotASecondNameOfTheRoot) {
    Routes routes = makeRoutes(kFiles);

    const HttpResponse response = get(routes, "/index.html");

    EXPECT_EQ(response.mStatus, 404U);
    expectSecurityHeadersWithoutCors(response);
}

TEST(ServerStaticFilesTest, UnknownAssetIsNotFound) {
    Routes routes = makeRoutes(kFiles);

    const HttpResponse response = get(routes, "/assets/other.js");

    EXPECT_EQ(response.mStatus, 404U);
    expectSecurityHeadersWithoutCors(response);
}

TEST(ServerStaticFilesTest, PathIsNotNormalised) {
    Routes routes = makeRoutes(kFiles);

    const HttpResponse response = get(routes, "/assets/../assets/app.js");

    EXPECT_EQ(response.mStatus, 404U);
    expectSecurityHeadersWithoutCors(response);
}

TEST(ServerStaticFilesTest, PostToAFileIsNotAllowed) {
    Routes routes = makeRoutes(kFiles);
    const HttpRequest request{.mMethod = "POST",
                              .mTarget = "/assets/app.js",
                              .mHeaders = {kHost, kOrigin}};

    const HttpResponse response = routes.handle(request);

    EXPECT_EQ(response.mStatus, 405U);
    EXPECT_EQ(headerOf(response, "Allow"), "GET, HEAD");
    expectSecurityHeadersWithoutCors(response);
}

TEST(ServerStaticFilesTest, PlaceholderTableServesOnlyThePlaceholderPage) {
    Routes routes = makeRoutes(placeholderStaticFiles());

    const HttpResponse page = get(routes, "/");
    const HttpResponse script = get(routes, "/assets/app.js");

    EXPECT_EQ(page.mStatus, 200U);
    EXPECT_EQ(headerOf(page, "Content-Type"), "text/html; charset=utf-8");
    EXPECT_NE(page.mBody.find("TaxBrokerReport teče."), std::string::npos);
    expectSecurityHeadersWithoutCors(page);

    EXPECT_EQ(script.mStatus, 404U);
    expectSecurityHeadersWithoutCors(script);
}

TEST(ServerStaticFilesTest, UnknownExtensionIsBinary) {
    EXPECT_EQ(contentTypeFor("assets/data.bin"), "application/octet-stream");
    EXPECT_EQ(contentTypeFor("noextension"), "application/octet-stream");
}

} // namespace
