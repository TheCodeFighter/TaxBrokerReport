#include "taxbroker/server/request_guard.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
// windows.h must come first.
#include <bcrypt.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__)
// macOS declares getentropy here, not in unistd.h.
#include <sys/random.h>
#endif

namespace taxbroker {

namespace {

constexpr std::size_t kTokenBytes = 32;

/// Fills the buffer from the operating system random source. Throws std::runtime_error when the
/// source fails; there is no fallback to another generator.
void fillRandomBytes(std::array<unsigned char, kTokenBytes>& aBuffer) {
#if defined(_WIN32)
    const NTSTATUS status = BCryptGenRandom(nullptr,
                                            aBuffer.data(),
                                            static_cast<ULONG>(aBuffer.size()),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    const bool succeeded = BCRYPT_SUCCESS(status);
#else
    // One call is enough: the size is below the 256 bytes getentropy accepts.
    const bool succeeded = getentropy(aBuffer.data(), aBuffer.size()) == 0;
#endif

    if (!succeeded)
    {
        throw std::runtime_error("no operating system random source");
    }
}

/// ASCII lower case of one character; other bytes are unchanged.
char toLowerAscii(char aCharacter) {
    if (aCharacter >= 'A' && aCharacter <= 'Z')
    {
        return static_cast<char>(aCharacter - 'A' + 'a');
    }

    return aCharacter;
}

/// True when the two texts are equal when ASCII letters are compared without case.
bool equalsIgnoringCase(std::string_view aFirst, std::string_view aSecond) {
    if (aFirst.size() != aSecond.size())
    {
        return false;
    }

    for (std::size_t index = 0; index < aFirst.size(); ++index)
    {
        if (toLowerAscii(aFirst[index]) != toLowerAscii(aSecond[index]))
        {
            return false;
        }
    }

    return true;
}

/// True when aValue is aPrefix + "127.0.0.1:" or "localhost:" + aPort, without case.
bool isLoopbackAuthority(std::string_view aValue, std::string_view aPrefix, std::uint16_t aPort) {
    const std::string port = std::to_string(aPort);

    return equalsIgnoringCase(aValue, std::string(aPrefix) + "127.0.0.1:" + port) ||
           equalsIgnoringCase(aValue, std::string(aPrefix) + "localhost:" + port);
}

/// Removes spaces and tabs at both ends.
std::string_view trimBlanks(std::string_view aText) {
    constexpr std::string_view kBlanks = " \t";

    const std::size_t first = aText.find_first_not_of(kBlanks);

    if (first == std::string_view::npos)
    {
        return {};
    }

    const std::size_t last = aText.find_last_not_of(kBlanks);

    return aText.substr(first, last - first + 1);
}

} // namespace

bool isAllowedHost(std::string_view aHost, std::uint16_t aPort) {
    return isLoopbackAuthority(aHost, "", aPort);
}

bool isAllowedOrigin(std::optional<std::string_view> aOrigin,
                     std::uint16_t aPort,
                     bool aStateChanging) {
    if (!aOrigin.has_value())
    {
        return !aStateChanging;
    }

    return isLoopbackAuthority(*aOrigin, "http://", aPort);
}

bool tokenMatches(std::string_view aExpected, std::string_view aGiven) {
    if (aExpected.empty() || aExpected.size() != aGiven.size())
    {
        return false;
    }

    // Collect every difference instead of stopping at the first one.
    unsigned char difference = 0;

    for (std::size_t index = 0; index < aExpected.size(); ++index)
    {
        difference =
            static_cast<unsigned char>(difference | (static_cast<unsigned char>(aExpected[index]) ^
                                                     static_cast<unsigned char>(aGiven[index])));
    }

    return difference == 0;
}

bool isStateChangingMethod(std::string_view aMethod) {
    return aMethod != "GET" && aMethod != "HEAD" && aMethod != "OPTIONS";
}

LinkToken::LinkToken(std::string aValue, std::chrono::steady_clock::time_point aIssuedAt)
    : mValue(std::move(aValue)), mIssuedAt(aIssuedAt) {}

const std::string& LinkToken::value() const {
    return mValue;
}

bool LinkToken::redeem(std::string_view aGiven, std::chrono::steady_clock::time_point aNow) {
    if (mRedeemed || aNow - mIssuedAt >= kLinkTokenLifetime || !tokenMatches(mValue, aGiven))
    {
        return false;
    }

    mRedeemed = true;

    return true;
}

std::string sessionCookieName(std::uint16_t aPort) {
    return "tbr_session_" + std::to_string(aPort);
}

std::string makeSessionCookie(std::string_view aToken, std::uint16_t aPort) {
    return sessionCookieName(aPort) + "=" + std::string(aToken) +
           "; HttpOnly; SameSite=Strict; Path=/";
}

std::optional<std::string_view> findSessionToken(std::string_view aCookieHeader,
                                                 std::uint16_t aPort) {
    const std::string name = sessionCookieName(aPort);

    std::optional<std::string_view> found;
    std::string_view rest = aCookieHeader;

    while (!rest.empty())
    {
        const std::size_t separator = rest.find(';');
        const std::string_view pair = trimBlanks(rest.substr(0, separator));
        rest =
            separator == std::string_view::npos ? std::string_view{} : rest.substr(separator + 1);

        const std::size_t equals = pair.find('=');

        // A pair without "=" or with another name is not ours.
        if (equals == std::string_view::npos || pair.substr(0, equals) != name)
        {
            continue;
        }

        if (found.has_value())
        {
            return std::nullopt;
        }

        found = pair.substr(equals + 1);
    }

    return found;
}

std::span<const HttpHeader> securityHeaders() {
    static constexpr std::array<HttpHeader, 7> kHeaders{{
        {"Content-Security-Policy",
         "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
         "connect-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; "
         "frame-ancestors 'none'"},
        {"X-Content-Type-Options", "nosniff"},
        {"Referrer-Policy", "no-referrer"},
        {"X-Frame-Options", "DENY"},
        {"Cross-Origin-Opener-Policy", "same-origin"},
        {"Cross-Origin-Resource-Policy", "same-origin"},
        {"Cache-Control", "no-store"},
    }};

    return kHeaders;
}

std::string makeToken() {
    constexpr std::string_view kHexDigits = "0123456789abcdef";

    std::array<unsigned char, kTokenBytes> bytes{};
    fillRandomBytes(bytes);

    std::string token;
    token.reserve(kTokenBytes * 2);

    for (const unsigned char byte : bytes)
    {
        token.push_back(kHexDigits[byte >> 4U]);
        token.push_back(kHexDigits[byte & 0x0FU]);
    }

    return token;
}

} // namespace taxbroker
