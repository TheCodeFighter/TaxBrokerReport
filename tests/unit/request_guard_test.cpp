#include "taxbroker/server/request_guard.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {
using namespace taxbroker;

TEST(RequestGuardTest, HostWithTheLoopbackAddressAndTheServerPortIsAllowed) {
    EXPECT_TRUE(isAllowedHost("127.0.0.1:8080", 8080));
}

TEST(RequestGuardTest, HostWithLocalhostAndTheServerPortIsAllowed) {
    EXPECT_TRUE(isAllowedHost("localhost:8080", 8080));
}

TEST(RequestGuardTest, HostWithAnotherPortIsRejected) {
    EXPECT_FALSE(isAllowedHost("127.0.0.1:8081", 8080));
}

TEST(RequestGuardTest, HostWithAForeignNameIsRejected) {
    // DNS rebinding: the browser sends the attacker's name with the real port.
    EXPECT_FALSE(isAllowedHost("evil.example:8080", 8080));
}

TEST(RequestGuardTest, EmptyHostIsRejected) {
    EXPECT_FALSE(isAllowedHost("", 8080));
}

TEST(RequestGuardTest, HostNameIsComparedWithoutCase) {
    EXPECT_TRUE(isAllowedHost("LOCALHOST:8080", 8080));
}

TEST(RequestGuardTest, HostWithoutAPortIsRejected) {
    EXPECT_FALSE(isAllowedHost("localhost", 8080));
}

TEST(RequestGuardTest, OriginOfTheServerIsAllowedForAStateChangingRequest) {
    EXPECT_TRUE(isAllowedOrigin("http://127.0.0.1:8080", 8080, true));
}

TEST(RequestGuardTest, OriginWithLocalhostIsAllowedForAStateChangingRequest) {
    EXPECT_TRUE(isAllowedOrigin("http://localhost:8080", 8080, true));
}

TEST(RequestGuardTest, OriginWithAnotherPortIsRejected) {
    EXPECT_FALSE(isAllowedOrigin("http://127.0.0.1:8081", 8080, false));
}

TEST(RequestGuardTest, OriginWithHttpsIsRejected) {
    // The local server speaks plain HTTP, so its own origin always starts with "http://".
    EXPECT_FALSE(isAllowedOrigin("https://127.0.0.1:8080", 8080, false));
}

TEST(RequestGuardTest, ForeignOriginIsRejected) {
    EXPECT_FALSE(isAllowedOrigin("http://evil.example", 8080, false));
}

TEST(RequestGuardTest, ForeignOriginIsRejectedForAStateChangingRequest) {
    EXPECT_FALSE(isAllowedOrigin("http://evil.example", 8080, true));
}

TEST(RequestGuardTest, NullOriginIsRejected) {
    // A sandboxed page or a local file sends the text "null".
    EXPECT_FALSE(isAllowedOrigin("null", 8080, false));
}

TEST(RequestGuardTest, MissingOriginIsRejectedForAStateChangingRequest) {
    EXPECT_FALSE(isAllowedOrigin(std::nullopt, 8080, true));
}

TEST(RequestGuardTest, MissingOriginIsAllowedForARequestThatChangesNothing) {
    EXPECT_TRUE(isAllowedOrigin(std::nullopt, 8080, false));
}

TEST(RequestGuardTest, EqualTokensMatch) {
    EXPECT_TRUE(tokenMatches("ab12", "ab12"));
}

TEST(RequestGuardTest, TokensWithADifferentLastCharacterDoNotMatch) {
    EXPECT_FALSE(tokenMatches("ab12", "ab13"));
}

TEST(RequestGuardTest, ShorterTokenDoesNotMatch) {
    EXPECT_FALSE(tokenMatches("ab12", "ab1"));
}

TEST(RequestGuardTest, EmptyExpectedTokenNeverMatches) {
    EXPECT_FALSE(tokenMatches("", ""));
}

TEST(RequestGuardTest, NewTokenIs64LowerCaseHexCharacters) {
    const std::string token = makeToken();

    EXPECT_EQ(token.size(), 64U); // 32 bytes, 2 hex characters each.
    EXPECT_TRUE(std::all_of(token.begin(), token.end(), [](char aCharacter) {
        return (aCharacter >= '0' && aCharacter <= '9') || (aCharacter >= 'a' && aCharacter <= 'f');
    }));
}

TEST(RequestGuardTest, TwoTokensAreDifferent) {
    EXPECT_NE(makeToken(), makeToken());
}

TEST(RequestGuardTest, GetIsNotStateChanging) {
    EXPECT_FALSE(isStateChangingMethod("GET"));
}

TEST(RequestGuardTest, HeadIsNotStateChanging) {
    EXPECT_FALSE(isStateChangingMethod("HEAD"));
}

TEST(RequestGuardTest, OptionsIsNotStateChanging) {
    EXPECT_FALSE(isStateChangingMethod("OPTIONS"));
}

TEST(RequestGuardTest, PostIsStateChanging) {
    EXPECT_TRUE(isStateChangingMethod("POST"));
}

TEST(RequestGuardTest, PatchIsStateChanging) {
    EXPECT_TRUE(isStateChangingMethod("PATCH"));
}

TEST(RequestGuardTest, LowerCaseGetIsStateChanging) {
    // Method names have case, so "get" is not the safe method GET.
    EXPECT_TRUE(isStateChangingMethod("get"));
}

TEST(RequestGuardTest, LinkTokenIsRedeemedOnce) {
    const auto issuedAt = std::chrono::steady_clock::time_point{std::chrono::seconds{1000}};
    LinkToken token{"ab12", issuedAt};

    EXPECT_TRUE(token.redeem("ab12", issuedAt + std::chrono::seconds{1}));
    EXPECT_FALSE(token.redeem("ab12", issuedAt + std::chrono::seconds{2}));
}

TEST(RequestGuardTest, LinkTokenIsRedeemedJustBeforeItsLifetimeEnds) {
    const auto issuedAt = std::chrono::steady_clock::time_point{std::chrono::seconds{1000}};
    LinkToken token{"ab12", issuedAt};

    EXPECT_TRUE(token.redeem("ab12", issuedAt + std::chrono::seconds{119}));
}

TEST(RequestGuardTest, LinkTokenExpiresAfter120Seconds) {
    const auto issuedAt = std::chrono::steady_clock::time_point{std::chrono::seconds{1000}};
    LinkToken token{"ab12", issuedAt};

    EXPECT_FALSE(token.redeem("ab12", issuedAt + std::chrono::seconds{120}));
}

TEST(RequestGuardTest, WrongLinkTokenDoesNotUseItUp) {
    const auto issuedAt = std::chrono::steady_clock::time_point{std::chrono::seconds{1000}};
    LinkToken token{"ab12", issuedAt};

    EXPECT_FALSE(token.redeem("ab13", issuedAt + std::chrono::seconds{1}));
    EXPECT_TRUE(token.redeem("ab12", issuedAt + std::chrono::seconds{2}));
}

TEST(RequestGuardTest, EmptyLinkTokenNeverRedeems) {
    const auto issuedAt = std::chrono::steady_clock::time_point{std::chrono::seconds{1000}};
    LinkToken token{"", issuedAt};

    EXPECT_FALSE(token.redeem("", issuedAt));
}

TEST(RequestGuardTest, LinkTokenKeepsItsValue) {
    const auto issuedAt = std::chrono::steady_clock::time_point{std::chrono::seconds{1000}};
    LinkToken token{"ab12", issuedAt};

    EXPECT_EQ(token.value(), "ab12");
}

TEST(RequestGuardTest, SessionCookieNameHasThePort) {
    EXPECT_EQ(sessionCookieName(8080), "tbr_session_8080");
}

TEST(RequestGuardTest, SessionCookieIsHttpOnlyAndStrict) {
    EXPECT_EQ(makeSessionCookie("ab12", 8080),
              "tbr_session_8080=ab12; HttpOnly; SameSite=Strict; Path=/");
}

TEST(RequestGuardTest, SessionTokenIsFoundAmongOtherCookies) {
    const std::optional<std::string_view> token =
        findSessionToken("theme=dark; tbr_session_8080=ab12; lang=sl", 8080);

    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(*token, "ab12");
}

TEST(RequestGuardTest, BlankCookiePairIsSkipped) {
    // The middle pair holds only a space.
    const std::optional<std::string_view> token =
        findSessionToken("theme=dark; ; tbr_session_8080=ab12", 8080);

    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(*token, "ab12");
}

TEST(RequestGuardTest, SessionTokenOfAnotherPortIsNotFound) {
    EXPECT_FALSE(findSessionToken("tbr_session_9090=ab12", 8080).has_value());
}

TEST(RequestGuardTest, MissingSessionCookieIsNotFound) {
    EXPECT_FALSE(findSessionToken("theme=dark", 8080).has_value());
}

TEST(RequestGuardTest, EmptyCookieHeaderIsNotFound) {
    EXPECT_FALSE(findSessionToken("", 8080).has_value());
}

TEST(RequestGuardTest, TwoSessionCookiesAreNotFound) {
    EXPECT_FALSE(
        findSessionToken("tbr_session_8080=ab12; tbr_session_8080=cd34", 8080).has_value());
}

TEST(RequestGuardTest, SessionCookieNameHasCase) {
    EXPECT_FALSE(findSessionToken("TBR_SESSION_8080=ab12", 8080).has_value());
}

TEST(RequestGuardTest, ClientHeaderIsXTaxBrokerOne) {
    EXPECT_EQ(kClientHeaderName, "X-TaxBroker");
    EXPECT_EQ(kClientHeaderValue, "1");
}

TEST(RequestGuardTest, SecurityHeadersAreTheSevenListed) {
    const std::span<const HttpHeader> headers = securityHeaders();

    ASSERT_EQ(headers.size(), 7U);

    EXPECT_EQ(headers[0].mName, "Content-Security-Policy");
    EXPECT_EQ(headers[0].mValue,
              "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
              "connect-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; "
              "frame-ancestors 'none'");
    EXPECT_EQ(headers[1].mName, "X-Content-Type-Options");
    EXPECT_EQ(headers[1].mValue, "nosniff");
    EXPECT_EQ(headers[2].mName, "Referrer-Policy");
    EXPECT_EQ(headers[2].mValue, "no-referrer");
    EXPECT_EQ(headers[3].mName, "X-Frame-Options");
    EXPECT_EQ(headers[3].mValue, "DENY");
    EXPECT_EQ(headers[4].mName, "Cross-Origin-Opener-Policy");
    EXPECT_EQ(headers[4].mValue, "same-origin");
    EXPECT_EQ(headers[5].mName, "Cross-Origin-Resource-Policy");
    EXPECT_EQ(headers[5].mValue, "same-origin");
    EXPECT_EQ(headers[6].mName, "Cache-Control");
    EXPECT_EQ(headers[6].mValue, "no-store");
}

} // namespace
