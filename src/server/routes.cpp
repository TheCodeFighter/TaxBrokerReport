#include "routes.hpp"

#include "taxbroker/logging.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace taxbroker {

namespace {

constexpr std::string_view kPlaceholderPage =
    "<!doctype html>\n"
    "<html lang=\"sl\"><head><meta charset=\"utf-8\"><title>TaxBrokerReport</title></head>\n"
    "<body><p>TaxBrokerReport teče. Uporabniški vmesnik pride v naslednji različici.</p>\n"
    "<p lang=\"en\">TaxBrokerReport is running. The user interface comes in a later "
    "version.</p></body></html>\n";

/// ASCII lower case of one character.
char toLowerAscii(char aCharacter) {
    if (aCharacter >= 'A' && aCharacter <= 'Z')
    {
        return static_cast<char>(aCharacter - 'A' + 'a');
    }

    return aCharacter;
}

/// True when the two names are equal without regard to case (header names).
bool sameName(std::string_view aFirst, std::string_view aSecond) {
    return std::equal(
        aFirst.begin(),
        aFirst.end(),
        aSecond.begin(),
        aSecond.end(),
        [](char aLeft, char aRight) { return toLowerAscii(aLeft) == toLowerAscii(aRight); });
}

/// The values of every header with this name, in the order sent.
std::vector<std::string_view> headerValues(const HttpRequest& aRequest, std::string_view aName) {
    std::vector<std::string_view> values;

    for (const HttpField& field : aRequest.mHeaders)
    {
        if (sameName(field.mName, aName))
        {
            values.push_back(field.mValue);
        }
    }

    return values;
}

/// A response with the security headers (R18) first. No `Access-Control-*` header is ever added
/// (R9).
HttpResponse makeResponse(unsigned aStatus, std::string aBody = {}) {
    HttpResponse response;
    response.mStatus = aStatus;
    response.mBody = std::move(aBody);

    for (const HttpHeader& header : securityHeaders())
    {
        response.mHeaders.push_back({std::string(header.mName), std::string(header.mValue)});
    }

    return response;
}

HttpResponse makeNotAllowedResponse(std::string_view aAllow) {
    HttpResponse response = makeResponse(405);
    response.mHeaders.push_back({"Allow", std::string(aAllow)});

    return response;
}

HttpResponse makeBadRequestResponse() {
    HttpResponse response = makeResponse(400, R"({"error":"bad_request"})");
    response.mHeaders.push_back({"Content-Type", "application/json"});

    return response;
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

/// True for "/api" and every path below "/api/".
bool isApiPath(std::string_view aPath) {
    return aPath == "/api" || aPath.starts_with("/api/");
}

} // namespace

HttpResponse makeErrorResponse(unsigned aStatus) {
    return makeResponse(aStatus);
}

Routes::Routes(std::uint16_t aPort,
               std::string aLinkToken,
               std::string aSessionToken,
               std::filesystem::path aLaunchFile,
               Clock aClock)
    : mPort(aPort), mLinkToken(std::move(aLinkToken), aClock()),
      mSessionToken(std::move(aSessionToken)), mLaunchFile(std::move(aLaunchFile)),
      mClock(std::move(aClock)) {}

HttpResponse Routes::handle(const HttpRequest& aRequest) {
    // Step 1: the Host header (R7).
    const std::vector<std::string_view> hosts = headerValues(aRequest, "Host");

    if (hosts.size() != 1 || !isAllowedHost(hosts.front(), mPort))
    {
        return makeResponse(403);
    }

    // The origin form is the only one a browser sends to a server; "*" and a full URL are not.
    if (aRequest.mTarget.empty() || aRequest.mTarget.front() != '/')
    {
        return makeBadRequestResponse();
    }

    const std::string_view path =
        std::string_view(aRequest.mTarget).substr(0, aRequest.mTarget.find('?'));

    // Step 2: the page and unknown paths need no login.
    if (!isApiPath(path))
    {
        if (path != "/")
        {
            return makeResponse(404);
        }

        if (aRequest.mMethod != "GET" && aRequest.mMethod != "HEAD")
        {
            return makeNotAllowedResponse("GET, HEAD");
        }

        HttpResponse page = makeResponse(200);
        page.mHeaders.push_back({"Content-Type", "text/html; charset=utf-8"});

        if (aRequest.mMethod == "GET")
        {
            page.mBody = std::string(kPlaceholderPage);
        }

        return page;
    }

    // Step 3: the Origin header (R8). Two Origin headers are never sent by a browser.
    const std::vector<std::string_view> origins = headerValues(aRequest, "Origin");

    if (origins.size() > 1)
    {
        return makeResponse(403);
    }

    std::optional<std::string_view> origin;

    if (!origins.empty())
    {
        origin = origins.front();
    }

    if (!isAllowedOrigin(origin, mPort, isStateChangingMethod(aRequest.mMethod)))
    {
        return makeResponse(403);
    }

    // Step 4: the client header (R17).
    const std::vector<std::string_view> clients = headerValues(aRequest, kClientHeaderName);

    if (clients.size() != 1 || clients.front() != kClientHeaderValue)
    {
        return makeResponse(403);
    }

    // Step 5: the login is the one API call without a session.
    if (path == "/api/session" && aRequest.mMethod == "POST")
    {
        return login(aRequest);
    }

    // Step 6: every other API call needs the session cookie (R5). An unknown path without it is
    // 401, not 404, so the paths cannot be discovered.
    const std::string cookies = joinedCookies(aRequest);
    const std::optional<std::string_view> session = findSessionToken(cookies, mPort);

    if (!session.has_value() || !tokenMatches(mSessionToken, *session))
    {
        return makeResponse(401);
    }

    // Step 7: the routes.
    if (path != "/api/session")
    {
        return makeResponse(404);
    }

    if (aRequest.mMethod != "GET")
    {
        return makeNotAllowedResponse("GET, POST");
    }

    return makeResponse(204);
}

HttpResponse Routes::login(const HttpRequest& aRequest) {
    const nlohmann::json body = nlohmann::json::parse(aRequest.mBody, nullptr, false);

    if (!body.is_object() || !body.contains("token") || !body["token"].is_string())
    {
        return makeBadRequestResponse();
    }

    if (!mLinkToken.redeem(body["token"].get<std::string>(), mClock()))
    {
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
