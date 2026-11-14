#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace taxbroker {

/// True when the `Host` header is `127.0.0.1:<aPort>` or `localhost:<aPort>`. The name is compared
/// without case; anything else (another name or port, an empty value) is false.
[[nodiscard]] bool isAllowedHost(std::string_view aHost, std::uint16_t aPort);

/// True when the `Origin` header is `http://127.0.0.1:<aPort>` or `http://localhost:<aPort>`. A
/// missing header is allowed only for a request that changes nothing (aStateChanging is false).
[[nodiscard]] bool
isAllowedOrigin(std::optional<std::string_view> aOrigin, std::uint16_t aPort, bool aStateChanging);

/// True when the two tokens are equal. The time does not depend on where they differ. An empty
/// expected token never matches.
[[nodiscard]] bool tokenMatches(std::string_view aExpected, std::string_view aGiven);

/// True for every method except GET, HEAD and OPTIONS. Methods have case, so "get" counts as
/// state changing.
[[nodiscard]] bool isStateChangingMethod(std::string_view aMethod);

/// How long a link token can be redeemed after it was made.
inline constexpr std::chrono::seconds kLinkTokenLifetime{120};

/// The one-time token of the browser link. It can be redeemed once, before its lifetime ends.
class LinkToken {
  public:
    /// Makes a token that was issued at aIssuedAt.
    LinkToken(std::string aValue, std::chrono::steady_clock::time_point aIssuedAt);

    /// The token text for the launch page.
    [[nodiscard]] const std::string& value() const;

    /// True once: when aGiven matches, it was not redeemed before and aNow is less than
    /// kLinkTokenLifetime after the issue time. A wrong token does not use it up.
    [[nodiscard]] bool redeem(std::string_view aGiven, std::chrono::steady_clock::time_point aNow);

  private:
    std::string mValue;
    std::chrono::steady_clock::time_point mIssuedAt;
    bool mRedeemed = false; ///< Not redeemed yet.
};

/// "tbr_session_<aPort>": the port keeps launches on other ports apart (cookies are per host).
[[nodiscard]] std::string sessionCookieName(std::uint16_t aPort);

/// The Set-Cookie value: "<name>=<aToken>; HttpOnly; SameSite=Strict; Path=/".
[[nodiscard]] std::string makeSessionCookie(std::string_view aToken, std::uint16_t aPort);

/// The value of the session cookie in a Cookie header, as a view into aCookieHeader. Empty when
/// it is missing or appears twice.
[[nodiscard]] std::optional<std::string_view> findSessionToken(std::string_view aCookieHeader,
                                                               std::uint16_t aPort);

inline constexpr std::string_view kClientHeaderName = "X-TaxBroker"; ///< Sent on every API call.
inline constexpr std::string_view kClientHeaderValue = "1";          ///< Its only valid value.

/// One response header.
struct HttpHeader {
    std::string_view mName;  ///< Header name.
    std::string_view mValue; ///< Header value.
};

/// The headers every response carries: content policy, no sniffing, no referrer, no framing,
/// same-origin isolation and no caching.
[[nodiscard]] std::span<const HttpHeader> securityHeaders();

/// A new secret token: 32 bytes from the operating system random source (`getentropy`, Windows
/// `BCryptGenRandom`), as 64 lower-case hex characters. Throws `std::runtime_error` when the
/// source fails.
[[nodiscard]] std::string makeToken();

} // namespace taxbroker
