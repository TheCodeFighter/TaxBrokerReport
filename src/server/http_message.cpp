#include "http_message.hpp"

#include "ascii_text.hpp"
#include "request_guard.hpp"

#include <utility>

namespace taxbroker {

HttpResponse makeResponse(unsigned aStatus, std::string aBody) {
    HttpResponse response;
    response.mStatus = aStatus;
    response.mBody = std::move(aBody);

    for (const HttpHeader& header : securityHeaders())
    {
        response.mHeaders.push_back({std::string(header.mName), std::string(header.mValue)});
    }

    return response;
}

HttpResponse makeErrorResponse(unsigned aStatus) {
    return makeResponse(aStatus);
}

HttpResponse makeJsonResponse(unsigned aStatus, std::string aBody) {
    HttpResponse response = makeResponse(aStatus, std::move(aBody));
    response.mHeaders.push_back({"Content-Type", "application/json"});

    return response;
}

HttpResponse makeErrorCodeResponse(unsigned aStatus, std::string_view aCode) {
    return makeJsonResponse(aStatus, R"({"error":")" + std::string(aCode) + R"("})");
}

HttpResponse makeNotAllowedResponse(std::string_view aAllow) {
    HttpResponse response = makeResponse(405);
    response.mHeaders.push_back({"Allow", std::string(aAllow)});

    return response;
}

std::vector<std::string_view> headerValues(const HttpRequest& aRequest, std::string_view aName) {
    std::vector<std::string_view> values;

    for (const HttpField& field : aRequest.mHeaders)
    {
        if (equalsIgnoringAsciiCase(field.mName, aName))
        {
            values.push_back(field.mValue);
        }
    }

    return values;
}

} // namespace taxbroker
