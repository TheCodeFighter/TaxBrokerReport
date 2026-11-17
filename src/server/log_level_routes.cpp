#include "log_level_routes.hpp"

#include "taxbroker/logging.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>

namespace taxbroker {

namespace {

using Json = nlohmann::json;

/// The longest body of a change: one key with one short word.
constexpr std::size_t kMaxBodyBytes = 1024;

HttpResponse refuse(unsigned aStatus, std::string_view aCode) {
    LOG_WARNING("Log level request refused: {} {}", aStatus, aCode);

    return makeErrorCodeResponse(aStatus, aCode);
}

/// `{"level":"error"}` for the level that the log has now.
HttpResponse levelResponse() {
    const Json body{{"level", selectableLogLevelName(currentLogLevel())}};

    return makeJsonResponse(200, body.dump());
}

/// The level of a body that is exactly one `level` key with one of the four words.
std::optional<spdlog::level::level_enum> levelOfBody(const std::string& aBody) {
    const Json body = Json::parse(aBody, nullptr, false);

    if (!body.is_object() || body.size() != 1 || !body.contains("level") ||
        !body.at("level").is_string())
    {
        return std::nullopt;
    }

    return parseSelectableLogLevel(body.at("level").get<std::string>());
}

HttpResponse change(const HttpRequest& aRequest) {
    if (aRequest.mBody.size() > kMaxBodyBytes)
    {
        LOG_WARNING("Log level request refused: 413 request_too_large (limit {})", kMaxBodyBytes);

        return makeErrorCodeResponse(413, "request_too_large");
    }

    const std::optional<spdlog::level::level_enum> level = levelOfBody(aRequest.mBody);

    if (!level.has_value())
    {
        return refuse(400, "bad_log_level");
    }

    // The new level is set before the line, so a change to info or debug shows its own line.
    setLogLevel(*level);
    LOG_INFO("Log level set: {}", selectableLogLevelName(*level));

    return levelResponse();
}

} // namespace

HttpResponse handleLogLevelRoute(const HttpRequest& aRequest, std::string_view aPath) {
    if (aPath != "/api/log-level")
    {
        return refuse(404, "not_found");
    }

    if (aRequest.mMethod == "GET")
    {
        return levelResponse();
    }

    if (aRequest.mMethod == "POST")
    {
        return change(aRequest);
    }

    LOG_WARNING("Log level request refused: 405 method_not_allowed");

    return makeNotAllowedResponse("GET, POST");
}

} // namespace taxbroker
