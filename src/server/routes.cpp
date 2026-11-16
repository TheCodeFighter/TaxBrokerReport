#include "routes.hpp"

#include "taxbroker/api/messages_json.hpp"
#include "taxbroker/logging.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace taxbroker {

namespace {

HttpResponse makeBadRequestResponse() {
    return makeErrorCodeResponse(400, "bad_request");
}

/// One warning for a request that the guard (or an unknown API path) refused: the status and a
/// fixed reason word. Never a header value, a target or a token, so the line can be sent with a
/// bug report.
void logRefusal(unsigned aStatus, std::string_view aReason) {
    LOG_WARNING("Request refused: {} {}", aStatus, aReason);
}

/// The Cookie headers as one text; a browser sends one, a client may send several.
std::string joinedCookies(const HttpRequest& aRequest) {
    std::string joined;

    for (const std::string_view value : headerValues(aRequest, "Cookie"))
    {
        if (!joined.empty())
        {
            joined += "; ";
        }

        joined += value;
    }

    return joined;
}

/// The path of a request target: the text before the `?`.
std::string_view pathOf(const std::string& aTarget) {
    return std::string_view(aTarget).substr(0, aTarget.find('?'));
}

/// The text after the `?` of a request target, or empty.
std::string_view queryOf(const std::string& aTarget) {
    const std::size_t mark = aTarget.find('?');

    return mark == std::string::npos ? std::string_view{}
                                     : std::string_view(aTarget).substr(mark + 1);
}

/// True for "/api" and every path below "/api/".
bool isApiPath(std::string_view aPath) {
    return aPath == "/api" || aPath.starts_with("/api/");
}

} // namespace

Routes::Routes(std::uint16_t aPort,
               std::string aLinkToken,
               std::string aSessionToken,
               std::filesystem::path aLaunchFile,
               Clock aClock,
               std::size_t aMaxFileBytes,
               std::span<const StaticFile> aStaticFiles,
               std::string_view aVersion)
    : mPort(aPort), mLinkToken(std::move(aLinkToken), aClock()),
      mSessionToken(std::move(aSessionToken)), mLaunchFile(std::move(aLaunchFile)),
      mClock(std::move(aClock)), mRunRoutes(aMaxFileBytes), mStaticFiles(aStaticFiles),
      mVersionBody(std::string{R"({"version":")"} + std::string{aVersion} + R"("})") {}

Routes::Verdict Routes::check(const HttpRequest& aRequest) const {
    // Step 1: the Host header (R7).
    const std::vector<std::string_view> hosts = headerValues(aRequest, "Host");

    if (hosts.size() != 1 || !isAllowedHost(hosts.front(), mPort))
    {
        return {Target::Refused, makeResponse(403), "host_not_allowed"};
    }

    // The origin form is the only one a browser sends to a server; "*" and a full URL are not.
    if (aRequest.mTarget.empty() || aRequest.mTarget.front() != '/')
    {
        return {Target::Refused, makeBadRequestResponse(), "bad_target"};
    }

    // Step 2: the page and unknown paths need no login.
    const std::string_view path = pathOf(aRequest.mTarget);

    if (!isApiPath(path))
    {
        return {Target::Page, {}};
    }

    // Step 3: the Origin header (R8). Two Origin headers are never sent by a browser.
    const std::vector<std::string_view> origins = headerValues(aRequest, "Origin");

    if (origins.size() > 1)
    {
        return {Target::Refused, makeResponse(403), "origin_repeated"};
    }

    std::optional<std::string_view> origin;

    if (!origins.empty())
    {
        origin = origins.front();
    }

    if (!isAllowedOrigin(origin, mPort, isStateChangingMethod(aRequest.mMethod)))
    {
        return {Target::Refused,
                makeResponse(403),
                origin.has_value() ? "origin_not_allowed" : "origin_missing"};
    }

    // Step 4: the client header (R17).
    const std::vector<std::string_view> clients = headerValues(aRequest, kClientHeaderName);

    if (clients.size() != 1 || clients.front() != kClientHeaderValue)
    {
        return {Target::Refused, makeResponse(403), "client_header"};
    }

    // Step 5: the login is the one API call without a session.
    if (path == "/api/session" && aRequest.mMethod == "POST")
    {
        return {Target::Login, {}};
    }

    // Step 6: every other API call needs the session cookie (R5). An unknown path without it is
    // 401, not 404, so the paths cannot be discovered.
    const std::string cookies = joinedCookies(aRequest);
    const std::optional<std::string_view> session = findSessionToken(cookies, mPort);

    if (!session.has_value())
    {
        return {Target::Refused, makeResponse(401), "no_session"};
    }

    if (!tokenMatches(mSessionToken, *session))
    {
        return {Target::Refused, makeResponse(401), "session_wrong"};
    }

    return {Target::Api, {}};
}

BodyPolicy Routes::bodyPolicy(const HttpRequest& aHeaderOnly) const {
    const Verdict verdict = check(aHeaderOnly);

    if (verdict.mTarget == Target::Refused)
    {
        // The server may send this answer at once for a large body; it writes the log line then.
        return {kDefaultBodyLimit, false, verdict.mRefusal, verdict.mReason};
    }

    if (verdict.mTarget == Target::Api && aHeaderOnly.mMethod == "POST" &&
        pathOf(aHeaderOnly.mTarget) == "/api/run")
    {
        return {mRunRoutes.uploadBodyLimit(), true};
    }

    return {kDefaultBodyLimit, true};
}

HttpResponse Routes::handle(const HttpRequest& aRequest) {
    const Verdict verdict = check(aRequest);

    if (verdict.mTarget == Target::Refused)
    {
        logRefusal(verdict.mRefusal.mStatus, verdict.mReason);

        return verdict.mRefusal;
    }

    const std::string_view path = pathOf(aRequest.mTarget);

    if (verdict.mTarget == Target::Page)
    {
        return staticFile(aRequest, path);
    }

    if (verdict.mTarget == Target::Login)
    {
        return login(aRequest);
    }

    // Step 7: the routes.
    if (path == "/api/run" || path.starts_with("/api/run/"))
    {
        return mRunRoutes.handle(aRequest, path, queryOf(aRequest.mTarget));
    }

    if (path == "/api/messages")
    {
        if (aRequest.mMethod != "GET")
        {
            logRefusal(405, "method_not_allowed");

            return makeNotAllowedResponse("GET");
        }

        return makeJsonResponse(200, api::messagesJson());
    }

    if (path == "/api/version")
    {
        if (aRequest.mMethod != "GET")
        {
            logRefusal(405, "method_not_allowed");

            return makeNotAllowedResponse("GET");
        }

        return makeJsonResponse(200, mVersionBody);
    }

    if (path != "/api/session")
    {
        logRefusal(404, "not_found");

        return makeResponse(404);
    }

    if (aRequest.mMethod != "GET")
    {
        logRefusal(405, "method_not_allowed");

        return makeNotAllowedResponse("GET, POST");
    }

    return makeResponse(204);
}

HttpResponse Routes::staticFile(const HttpRequest& aRequest, std::string_view aPath) const {
    // The path must match exactly: "/" is the page, and "/index.html" is not a second name for it.
    // There is no normalising, so "/assets/../assets/app.js" is not found.
    constexpr std::string_view kPage = "index.html";
    const std::string_view name = aPath == "/" ? kPage : aPath.substr(1);
    const auto found =
        std::find_if(mStaticFiles.begin(), mStaticFiles.end(), [name](const StaticFile& aFile) {
            return aFile.mPath == name;
        });

    if (found == mStaticFiles.end() || (aPath != "/" && name == kPage))
    {
        return makeResponse(404);
    }

    if (aRequest.mMethod != "GET" && aRequest.mMethod != "HEAD")
    {
        return makeNotAllowedResponse("GET, HEAD");
    }

    HttpResponse response = makeResponse(200);
    response.mHeaders.push_back({"Content-Type", std::string{contentTypeFor(found->mPath)}});

    if (aRequest.mMethod == "GET")
    {
        response.mBody = std::string(found->mBody);
    }

    return response;
}

HttpResponse Routes::login(const HttpRequest& aRequest) {
    const nlohmann::json body = nlohmann::json::parse(aRequest.mBody, nullptr, false);

    if (!body.is_object() || !body.contains("token") || !body["token"].is_string())
    {
        LOG_WARNING("Login refused: 400 bad_request");

        return makeBadRequestResponse();
    }

    const LinkRedeem redeemed = mLinkToken.redeem(body["token"].get<std::string>(), mClock());

    if (redeemed != LinkRedeem::Redeemed)
    {
        // Used, expired or wrong: the word tells the user which link problem to look for.
        LOG_WARNING("Login refused: 401 {}", linkRedeemWord(redeemed));

        return makeResponse(401);
    }

    // The link is used up: the file would only show an expired link (R19).
    std::error_code ignored;
    std::filesystem::remove(mLaunchFile, ignored);

    LOG_INFO("Browser login done");

    HttpResponse response = makeResponse(204);
    response.mHeaders.push_back({"Set-Cookie", makeSessionCookie(mSessionToken, mPort)});

    return response;
}

} // namespace taxbroker
