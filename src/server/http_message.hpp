#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace taxbroker {

/// The longest body of any request except an upload: the login and the answers are small.
inline constexpr std::uint64_t kDefaultBodyLimit = 1024 * 1024;

/// One header of a request or a response.
struct HttpField {
    std::string mName;  ///< Header name, as sent.
    std::string mValue; ///< Header value.
};

/// A request as the routes see it. It holds no socket.
struct HttpRequest {
    std::string mMethod;             ///< `GET`, `POST`, ...; always set by the creator.
    std::string mTarget;             ///< Path and query, as sent.
    std::vector<HttpField> mHeaders; ///< All headers, in the order sent.
    std::string mBody;               ///< The body; empty when there is none.
};

/// A response before it is written. Every one carries the security headers.
struct HttpResponse {
    unsigned mStatus{};              ///< HTTP status code; always set by the creator.
    std::vector<HttpField> mHeaders; ///< Security headers first, then the route's own.
    std::string mBody;               ///< The body; empty when there is none.
};

/// How much body the server may read for a request, decided from its header alone.
struct BodyPolicy {
    std::uint64_t mLimit{}; ///< Longest body in bytes; always set.
    bool mGuardPassed{}; ///< The guard accepts the header, so `Expect: 100-continue` is answered.
    /// The guard's own answer. The server sends it, instead of a 413, when the body is large or
    /// has no declared length, so it never reads the body of a refused request. Empty: none.
    std::optional<HttpResponse> mRefusal;
    /// The fixed word of the refusal for the log (`session_wrong`, ...); empty without a refusal.
    std::string_view mReason{};
};

/// A response with the security headers (R18) first. No `Access-Control-*` header is ever added
/// (R9).
[[nodiscard]] HttpResponse makeResponse(unsigned aStatus, std::string aBody = {});

/// An error response with the security headers and no body detail: 400, 413, 431 and 500 come
/// from the HTTP layer, the rest from the routes.
[[nodiscard]] HttpResponse makeErrorResponse(unsigned aStatus);

/// A response with a JSON body and the security headers.
[[nodiscard]] HttpResponse makeJsonResponse(unsigned aStatus, std::string aBody);

/// A JSON error response `{"error":"<aCode>"}`; the code is a stable word for the frontend.
[[nodiscard]] HttpResponse makeErrorCodeResponse(unsigned aStatus, std::string_view aCode);

/// A 405 response that names the allowed methods, for example `GET, POST`.
[[nodiscard]] HttpResponse makeNotAllowedResponse(std::string_view aAllow);

/// The values of every header with this name (compared without case), in the order sent.
[[nodiscard]] std::vector<std::string_view> headerValues(const HttpRequest& aRequest,
                                                         std::string_view aName);

} // namespace taxbroker
