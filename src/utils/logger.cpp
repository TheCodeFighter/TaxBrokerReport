#include "utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <spdlog/sinks/basic_file_sink.h>

namespace {

constexpr auto kLoggerName = "taxbroker";
constexpr auto kDefaultLogFilePath = "logs/taxbroker.log";
constexpr auto kDefaultPattern = "[%Y-%m-%d %H:%M:%S.%e] [tid %2t] [%^%-8l%$] [%-20!s:%4#] %v";

std::string ToLower(std::string_view aValue) {
    std::string loweredValue{aValue};
    std::transform(
        loweredValue.begin(), loweredValue.end(), loweredValue.begin(),
        [](unsigned char aCharacter) { return static_cast<char>(std::tolower(aCharacter)); });
    return loweredValue;
}

const char* GetEnvironmentValue(const char* aName) {
    const char* value = std::getenv(aName);
    if (value == nullptr || *value == '\0') {
        return nullptr;
    }

    return value;
}

std::optional<spdlog::level::level_enum> ParseLogLevel(const char* aValue) {
    if (aValue == nullptr) {
        return std::nullopt;
    }

    const auto normalizedValue = ToLower(aValue);
    if (normalizedValue == "trace") {
        return spdlog::level::trace;
    }
    if (normalizedValue == "debug") {
        return spdlog::level::debug;
    }
    if (normalizedValue == "info") {
        return spdlog::level::info;
    }
    if (normalizedValue == "warn" || normalizedValue == "warning") {
        return spdlog::level::warn;
    }
    if (normalizedValue == "error" || normalizedValue == "err") {
        return spdlog::level::err;
    }
    if (normalizedValue == "critical" || normalizedValue == "crit") {
        return spdlog::level::critical;
    }
    if (normalizedValue == "off") {
        return spdlog::level::off;
    }

    return std::nullopt;
}

std::optional<bool> ParseLogFileMode(const char* aValue) {
    if (aValue == nullptr) {
        return std::nullopt;
    }

    const auto normalizedValue = ToLower(aValue);
    if (normalizedValue == "append" || normalizedValue == "a" || normalizedValue == "false" ||
        normalizedValue == "0" || normalizedValue == "no") {
        return false;
    }

    if (normalizedValue == "truncate" || normalizedValue == "t" || normalizedValue == "true" ||
        normalizedValue == "1" || normalizedValue == "yes" || normalizedValue == "overwrite") {
        return true;
    }

    return std::nullopt;
}

std::optional<std::string> GetLogFilePath(const std::shared_ptr<spdlog::logger>& aLogger) {
    if (aLogger == nullptr) {
        return std::nullopt;
    }

    for (const auto& sink : aLogger->sinks()) {
        const auto fileSink = std::dynamic_pointer_cast<spdlog::sinks::basic_file_sink_mt>(sink);
        if (fileSink != nullptr) {
            return std::string{fileSink->filename()};
        }
    }

    return std::nullopt;
}

} // namespace

namespace taxbroker {

void InitializeLogger() {
    const auto logLevelValue = GetEnvironmentValue("TBR_LOG_LEVEL");
    const auto logFileModeValue = GetEnvironmentValue("TBR_LOG_FILE_MODE");
    const auto logFilePathValue = GetEnvironmentValue("TBR_LOG_FILE");

    const std::string logFilePath = logFilePathValue != nullptr ? std::string{logFilePathValue}
                                                                : std::string{kDefaultLogFilePath};
    const bool truncateLogFile = ParseLogFileMode(logFileModeValue).value_or(false);

    auto logger = spdlog::get(kLoggerName);
    if (logger == nullptr || GetLogFilePath(logger).value_or(std::string{}) != logFilePath) {
        spdlog::drop(kLoggerName);
        logger = spdlog::basic_logger_mt(kLoggerName, logFilePath, truncateLogFile);
    }

    spdlog::set_default_logger(logger);

    spdlog::set_level(ParseLogLevel(logLevelValue).value_or(spdlog::level::debug));

    spdlog::flush_on(spdlog::level::warn);

    spdlog::set_pattern(kDefaultPattern);
}

} // namespace taxbroker