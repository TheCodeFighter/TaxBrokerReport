#include "taxbroker/logging.hpp"

#include "taxbroker/state_folder.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace {

bool IsStdoutEnabledByDefault() {
#ifndef NDEBUG
    return true;
#else
    return false;
#endif
}

constexpr auto kLoggerName = "taxbroker";
constexpr auto kLogFileName = "taxbroker.log";
constexpr auto kDefaultPattern = "[%Y-%m-%d %H:%M:%S.%e] [tid %2t] [%^%-8l%$] [%-20!s:%4#] %v";
constexpr std::size_t kAsyncQueueSize = 8192;
constexpr std::size_t kAsyncThreadCount = 1;
constexpr std::size_t kMaxAsyncQueueSize = 65536;
constexpr std::size_t kMaxAsyncThreadCount = 8;

std::string ToLower(std::string_view aValue) {
    std::string loweredValue{aValue};
    std::transform(
        loweredValue.begin(),
        loweredValue.end(),
        loweredValue.begin(),
        [](unsigned char aCharacter) { return static_cast<char>(std::tolower(aCharacter)); });
    return loweredValue;
}

const char* GetEnvironmentValue(const char* aName) {
    const char* value = std::getenv(aName);
    if (value == nullptr || *value == '\0')
    {
        return nullptr;
    }

    return value;
}

std::optional<spdlog::level::level_enum> ParseLogLevel(const char* aValue) {
    if (aValue == nullptr)
    {
        return std::nullopt;
    }

    const auto normalizedValue = ToLower(aValue);
    if (normalizedValue == "trace")
    {
        return spdlog::level::trace;
    }
    if (normalizedValue == "debug")
    {
        return spdlog::level::debug;
    }
    if (normalizedValue == "info")
    {
        return spdlog::level::info;
    }
    if (normalizedValue == "warn" || normalizedValue == "warning")
    {
        return spdlog::level::warn;
    }
    if (normalizedValue == "error" || normalizedValue == "err")
    {
        return spdlog::level::err;
    }
    if (normalizedValue == "critical" || normalizedValue == "crit")
    {
        return spdlog::level::critical;
    }
    if (normalizedValue == "off")
    {
        return spdlog::level::off;
    }

    return std::nullopt;
}

std::optional<bool> ParseLogFileMode(const char* aValue) {
    if (aValue == nullptr)
    {
        return std::nullopt;
    }

    const auto normalizedValue = ToLower(aValue);
    if (normalizedValue == "append" || normalizedValue == "a" || normalizedValue == "false" ||
        normalizedValue == "0" || normalizedValue == "no")
    {
        return false;
    }

    if (normalizedValue == "truncate" || normalizedValue == "t" || normalizedValue == "true" ||
        normalizedValue == "1" || normalizedValue == "yes" || normalizedValue == "overwrite")
    {
        return true;
    }

    return std::nullopt;
}

std::optional<bool> ParseLogBool(const char* aValue) {
    if (aValue == nullptr)
    {
        return std::nullopt;
    }

    const auto normalizedValue = ToLower(aValue);
    if (normalizedValue == "1" || normalizedValue == "true")
    {
        return true;
    }
    if (normalizedValue == "0" || normalizedValue == "false")
    {
        return false;
    }

    return std::nullopt;
}

// NOTE:
// - Value must be > 0
// - Value must not exceed aMaxValue
// - Overflow (ERANGE) is rejected
std::optional<std::size_t> ParseSizeT(const char* aValue, std::size_t aMaxValue) {
    if (aValue == nullptr)
    {
        return std::nullopt;
    }

    errno = 0;
    char* end = nullptr;
    const auto parsedValue = std::strtoul(aValue, &end, 10);
    if (end == aValue || *end != '\0' || errno == ERANGE)
    {
        return std::nullopt;
    }

    if (parsedValue == 0U)
    {
        return std::nullopt;
    }

    const auto maxSizeT = static_cast<unsigned long>(std::numeric_limits<std::size_t>::max());
    if (parsedValue > maxSizeT || parsedValue > static_cast<unsigned long>(aMaxValue))
    {
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
    if (aLogger == nullptr)
    {
        return false;
    }

    return std::dynamic_pointer_cast<spdlog::async_logger>(aLogger) != nullptr;
}

std::optional<std::string> SinkFilePath(const spdlog::sink_ptr& aSink) {
    const auto fileSink = std::dynamic_pointer_cast<spdlog::sinks::rotating_file_sink_mt>(aSink);

    if (fileSink == nullptr)
    {
        return std::nullopt;
    }

    return std::string{fileSink->filename()};
}

std::optional<std::string> GetLogFilePath(const std::shared_ptr<spdlog::logger>& aLogger) {
    if (aLogger == nullptr)
    {
        return std::nullopt;
    }

    for (const auto& sink : aLogger->sinks())
    {
        const auto path = SinkFilePath(sink);

        if (path.has_value())
        {
            return path;
        }
    }

    return std::nullopt;
}

} // namespace

namespace taxbroker {

spdlog::level::level_enum defaultLogLevel() {
    return spdlog::level::err;
}

std::optional<spdlog::level::level_enum> parseSelectableLogLevel(std::string_view aName) {
    if (aName == "error")
    {
        return spdlog::level::err;
    }
    if (aName == "warn")
    {
        return spdlog::level::warn;
    }
    if (aName == "info")
    {
        return spdlog::level::info;
    }
    if (aName == "debug")
    {
        return spdlog::level::debug;
    }

    return std::nullopt;
}

std::string_view selectableLogLevelName(spdlog::level::level_enum aLevel) {
    switch (aLevel)
    {
    case spdlog::level::trace:
    case spdlog::level::debug:
        return "debug";
    case spdlog::level::info:
        return "info";
    case spdlog::level::warn:
        return "warn";
    case spdlog::level::err:
    case spdlog::level::critical:
    case spdlog::level::off:
    case spdlog::level::n_levels:
        return "error";
    }

    return "error";
}

void setLogLevel(spdlog::level::level_enum aLevel) {
    spdlog::default_logger()->set_level(aLevel);
}

spdlog::level::level_enum currentLogLevel() {
    return spdlog::default_logger()->level();
}

std::optional<std::filesystem::path> logFilePath(const EnvLookup& aEnv) {
    const std::optional<std::string> explicitFile = aEnv("TBR_LOG_FILE");

    if (explicitFile.has_value() && !explicitFile->empty())
    {
        return std::filesystem::path{*explicitFile};
    }

    try
    { return stateFolder(aEnv) / kLogFileName; } catch (const std::exception&)
    {
        // No absolute HOME: there is no state folder, so no log file.
        return std::nullopt;
    }
}

std::optional<std::filesystem::path> currentLogFilePath() {
    const std::optional<std::string> file = GetLogFilePath(spdlog::default_logger());

    if (!file.has_value())
    {
        return std::nullopt;
    }

    return std::filesystem::path{*file};
}

spdlog::sink_ptr
makeLogFileSink(const std::filesystem::path& aFile, std::size_t aMaxBytes, std::size_t aMaxFiles) {
    return std::make_shared<spdlog::sinks::rotating_file_sink_mt>(aFile.string(),
                                                                  aMaxBytes,
                                                                  aMaxFiles);
}

spdlog::sink_ptr openLogFile(const EnvLookup& aEnv, bool aTruncate) {
    const std::optional<std::filesystem::path> file = logFilePath(aEnv);

    if (!file.has_value())
    {
        return nullptr;
    }

    try
    {
        // Only the default folder is made here; a folder of TBR_LOG_FILE is the user's own.
        const std::optional<std::string> explicitFile = aEnv("TBR_LOG_FILE");

        if (!explicitFile.has_value() || explicitFile->empty())
        {
            makePrivateFolder(file->parent_path());
        }

        if (aTruncate)
        {
            std::error_code removeError;

            std::filesystem::remove(*file, removeError);
        }

        return makeLogFileSink(*file, kLogFileMaxBytes, kLogFileCount);
    } catch (const std::exception&)
    {
        // The text of the exception can hold the path, so it is not logged.
        return nullptr;
    }
}

std::vector<std::string> InvalidLogSettings() {
    std::vector<std::string> invalid;

    const auto check = [&invalid](const char* aName, bool aIsValid) {
        if (GetEnvironmentValue(aName) != nullptr && !aIsValid)
        {
            invalid.emplace_back(aName);
        }
    };

    check("TBR_LOG_LEVEL", ParseLogLevel(GetEnvironmentValue("TBR_LOG_LEVEL")).has_value());
    check("TBR_LOG_FILE_MODE",
          ParseLogFileMode(GetEnvironmentValue("TBR_LOG_FILE_MODE")).has_value());
    check("TBR_LOG_ASYNC", ParseLogBool(GetEnvironmentValue("TBR_LOG_ASYNC")).has_value());
    check("TBR_LOG_QUEUE_SIZE",
          ParseSizeT(GetEnvironmentValue("TBR_LOG_QUEUE_SIZE"), kMaxAsyncQueueSize).has_value());
    check("TBR_LOG_THREADS",
          ParseSizeT(GetEnvironmentValue("TBR_LOG_THREADS"), kMaxAsyncThreadCount).has_value());
    check("TBR_LOG_STDOUT", ParseLogBool(GetEnvironmentValue("TBR_LOG_STDOUT")).has_value());

    return invalid;
}

void InitializeLogger() {
    static std::once_flag sInitializeOnce;
    std::call_once(sInitializeOnce, []() {
        const auto logLevelValue = GetEnvironmentValue("TBR_LOG_LEVEL");
        const auto logFileModeValue = GetEnvironmentValue("TBR_LOG_FILE_MODE");
        const auto logAsyncValue = GetEnvironmentValue("TBR_LOG_ASYNC");
        const auto logQueueSizeValue = GetEnvironmentValue("TBR_LOG_QUEUE_SIZE");
        const auto logThreadCountValue = GetEnvironmentValue("TBR_LOG_THREADS");
        const auto logStdoutValue = GetEnvironmentValue("TBR_LOG_STDOUT");

        const bool truncateLogFile = ParseLogFileMode(logFileModeValue).value_or(false);
        const bool useAsyncLogger = ParseLogBool(logAsyncValue).value_or(true);
        const std::size_t logQueueSize =
            ParseSizeT(logQueueSizeValue, kMaxAsyncQueueSize).value_or(kAsyncQueueSize);
        const std::size_t logThreadCount =
            ParseSizeT(logThreadCountValue, kMaxAsyncThreadCount).value_or(kAsyncThreadCount);
        const bool useStdoutSink =
            ParseLogBool(logStdoutValue).value_or(IsStdoutEnabledByDefault());

        // A file that cannot be opened is reported after the logger exists; the run goes on
        // without a log file.
        const spdlog::sink_ptr fileSink = openLogFile(systemEnvironment(), truncateLogFile);
        const std::string logFilePathText = SinkFilePath(fileSink).value_or(std::string{});

        auto logger = spdlog::get(kLoggerName);
        if (logger == nullptr ||
            GetLogFilePath(logger).value_or(std::string{}) != logFilePathText ||
            IsAsyncLogger(logger) != useAsyncLogger)
        {
            spdlog::drop(kLoggerName);

            std::vector<spdlog::sink_ptr> sinks;
            sinks.reserve(2U);

            if (useStdoutSink)
            {
                sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
            }

            if (fileSink != nullptr)
            {
                sinks.push_back(fileSink);
            }

            if (useAsyncLogger)
            {
                EnsureAsyncThreadPoolInitialized(logQueueSize, logThreadCount);
                logger =
                    std::make_shared<spdlog::async_logger>(kLoggerName,
                                                           sinks.begin(),
                                                           sinks.end(),
                                                           spdlog::thread_pool(),
                                                           spdlog::async_overflow_policy::block);
            }
            else
            {
                logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
            }

            spdlog::register_logger(logger);
        }

        spdlog::set_default_logger(logger);

        spdlog::set_level(ParseLogLevel(logLevelValue).value_or(defaultLogLevel()));

        spdlog::flush_on(spdlog::level::warn);
        spdlog::flush_every(std::chrono::seconds(1));

        spdlog::set_pattern(kDefaultPattern);

        if (fileSink == nullptr)
        {
            LOG_WARNING("No log file is written: the log file cannot be opened or there is no "
                        "state folder");
        }

        // A wrong setting is ignored, but the log says so. Names only: a value may be a path.
        for (const std::string& name : InvalidLogSettings())
        {
            LOG_WARNING("Log setting {} is not valid and is ignored: the default is used", name);
        }
    });
}

} // namespace taxbroker
