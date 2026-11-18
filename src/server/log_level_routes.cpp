#include "log_level_routes.hpp"

#include "taxbroker/logging.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/sinks/rotating_file_sink.h>

#include <cstddef>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

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

/// The `404 no_log_file` answer: there is nothing to save yet.
HttpResponse noLogFile() {
    LOG_WARNING("Log request refused: 404 no_log_file");

    return makeErrorCodeResponse(404, "no_log_file");
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

std::string readLogTail(const std::filesystem::path& aFile, std::size_t aMaxBytes) {
    std::ifstream stream(aFile, std::ios::binary | std::ios::ate);

    if (!stream)
    {
        return {};
    }

    const std::streamoff size = stream.tellg();

    if (size <= 0)
    {
        return {};
    }

    const auto total = static_cast<std::size_t>(size);
    const std::size_t start = total > aMaxBytes ? total - aMaxBytes : 0;
    std::string text(total - start, '\0');

    stream.seekg(static_cast<std::streamoff>(start));
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(stream.gcount()));

    if (start == 0)
    {
        return text;
    }

    // The cut is inside a line: drop it, so the text starts at a whole line.
    const std::size_t lineEnd = text.find('\n');

    if (lineEnd == std::string::npos)
    {
        return {};
    }

    text.erase(0, lineEnd + 1);

    return text;
}

HttpResponse handleLogFileRoute(const HttpRequest& aRequest,
                                const std::optional<std::filesystem::path>& aLogFile) {
    if (aRequest.mMethod != "GET")
    {
        LOG_WARNING("Log request refused: 405 method_not_allowed");

        return makeNotAllowedResponse("GET");
    }

    if (!aLogFile.has_value())
    {
        return noLogFile();
    }

    // The logger may be asynchronous: this asks for a write, so the last second can be missing.
    spdlog::default_logger()->flush();

    const std::filesystem::path older{
        spdlog::sinks::rotating_file_sink_mt::calc_filename(aLogFile->string(), 1)};
    std::string body = readLogTail(older, kLogFileMaxBytes);

    body += readLogTail(*aLogFile, kLogFileMaxBytes);

    if (body.empty())
    {
        return noLogFile();
    }

    // Logged after the read, so this line is not in the file that is sent.
    LOG_INFO("Log downloaded: {} bytes", body.size());

    HttpResponse response = makeResponse(200, std::move(body));

    response.mHeaders.push_back({"Content-Type", "text/plain; charset=utf-8"});
    response.mHeaders.push_back(
        {"Content-Disposition", "attachment; filename=\"taxbroker-log.txt\""});

    return response;
}

} // namespace taxbroker
