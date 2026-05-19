#include "utils/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace {

spdlog::level::level_enum DefaultLogLevel() {
#ifndef NDEBUG
    return spdlog::level::debug;
#else
    return spdlog::level::info;
#endif
}

bool IsStdoutEnabledByDefault() {
#ifndef NDEBUG
    return true;
#else
    return false;
#endif
}

constexpr auto kLoggerName = "taxbroker";
constexpr auto kDefaultLogFilePath = "logs/taxbroker.log";
constexpr auto kDefaultPattern = "[%Y-%m-%d %H:%M:%S.%e] [tid %2t] [%^%-8l%$] [%-20!s:%4#] %v";
constexpr std::size_t kAsyncQueueSize = 8192;
constexpr std::size_t kAsyncThreadCount = 1;
constexpr std::size_t kMaxAsyncQueueSize = 65536;
constexpr std::size_t kMaxAsyncThreadCount = 8;

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

std::optional<bool> ParseLogBool(const char* aValue) {
    if (aValue == nullptr) {
        return std::nullopt;
    }

    const auto normalizedValue = ToLower(aValue);
    if (normalizedValue == "1" || normalizedValue == "true") {
        return true;
    }
    if (normalizedValue == "0" || normalizedValue == "false") {
        return false;
    }

    return std::nullopt;
}

// NOTE:
// - Value must be > 0
// - Value must not exceed aMaxValue
// - Overflow (ERANGE) is rejected
std::optional<std::size_t> ParseSizeT(const char* aValue, std::size_t aMaxValue) {
    if (aValue == nullptr) {
        return std::nullopt;
    }

    errno = 0;
    char* end = nullptr;
    const auto parsedValue = std::strtoul(aValue, &end, 10);
    if (end == aValue || *end != '\0' || errno == ERANGE) {
        return std::nullopt;
    }

    if (parsedValue == 0U) {
        return std::nullopt;
    }

    const auto maxSizeT = static_cast<unsigned long>(std::numeric_limits<std::size_t>::max());
    if (parsedValue > maxSizeT || parsedValue > static_cast<unsigned long>(aMaxValue)) {
        return std::nullopt;
    }

    return static_cast<std::size_t>(parsedValue);
}

// NOTE:
// Thread pool is initialized only once.
// Queue size and thread count are taken from the first call.
// Runtime changes via environment variables are NOT applied.
void EnsureAsyncThreadPoolInitialized(std::size_t aQueueSize, std::size_t aThreadCount) {
    static std::once_flag sThreadPoolOnce;
    std::call_once(sThreadPoolOnce, [aQueueSize, aThreadCount]() {
        spdlog::init_thread_pool(aQueueSize, aThreadCount);
    });
}

bool IsAsyncLogger(const std::shared_ptr<spdlog::logger>& aLogger) {
    if (aLogger == nullptr) {
        return false;
    }

    return std::dynamic_pointer_cast<spdlog::async_logger>(aLogger) != nullptr;
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
    static std::once_flag sInitializeOnce;
    std::call_once(sInitializeOnce, []() {
        const auto logLevelValue = GetEnvironmentValue("TBR_LOG_LEVEL");
        const auto logFileModeValue = GetEnvironmentValue("TBR_LOG_FILE_MODE");
        const auto logFilePathValue = GetEnvironmentValue("TBR_LOG_FILE");
        const auto logAsyncValue = GetEnvironmentValue("TBR_LOG_ASYNC");
        const auto logQueueSizeValue = GetEnvironmentValue("TBR_LOG_QUEUE_SIZE");
        const auto logThreadCountValue = GetEnvironmentValue("TBR_LOG_THREADS");
        const auto logStdoutValue = GetEnvironmentValue("TBR_LOG_STDOUT");

        const std::string logFilePath = logFilePathValue != nullptr
                                            ? std::string{logFilePathValue}
                                            : std::string{kDefaultLogFilePath};
        const bool truncateLogFile = ParseLogFileMode(logFileModeValue).value_or(false);
        const bool useAsyncLogger = ParseLogBool(logAsyncValue).value_or(true);
        const std::size_t logQueueSize =
            ParseSizeT(logQueueSizeValue, kMaxAsyncQueueSize).value_or(kAsyncQueueSize);
        const std::size_t logThreadCount =
            ParseSizeT(logThreadCountValue, kMaxAsyncThreadCount).value_or(kAsyncThreadCount);
        const bool useStdoutSink =
            ParseLogBool(logStdoutValue).value_or(IsStdoutEnabledByDefault());

        auto logger = spdlog::get(kLoggerName);
        if (logger == nullptr || GetLogFilePath(logger).value_or(std::string{}) != logFilePath ||
            IsAsyncLogger(logger) != useAsyncLogger) {
            spdlog::drop(kLoggerName);

            std::vector<spdlog::sink_ptr> sinks;
            sinks.reserve(useStdoutSink ? 2U : 1U);

            if (useStdoutSink) {
                sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
            }

            sinks.push_back(
                std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath, truncateLogFile));

            if (useAsyncLogger) {
                EnsureAsyncThreadPoolInitialized(logQueueSize, logThreadCount);
                logger = std::make_shared<spdlog::async_logger>(
                    kLoggerName, sinks.begin(), sinks.end(), spdlog::thread_pool(),
                    spdlog::async_overflow_policy::block);
            } else {
                logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
            }

            spdlog::register_logger(logger);
        }

        spdlog::set_default_logger(logger);

        spdlog::set_level(ParseLogLevel(logLevelValue).value_or(DefaultLogLevel()));

        spdlog::flush_on(spdlog::level::warn);
        spdlog::flush_every(std::chrono::seconds(1));

        spdlog::set_pattern(kDefaultPattern);
    });
}

} // namespace taxbroker