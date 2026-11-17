#pragma once

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

#include "taxbroker/state_folder.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace taxbroker {

/// The size of one log file before it rotates (5 MiB).
inline constexpr std::size_t kLogFileMaxBytes = 5U * 1024U * 1024U;

/// The number of rotated log files that are kept besides the current one.
inline constexpr std::size_t kLogFileCount = 3;

void InitializeLogger();

/// The level when `TBR_LOG_LEVEL` is not set: `error`, for every build.
[[nodiscard]] spdlog::level::level_enum defaultLogLevel();

/// One of the four levels a user can choose, by its exact lower-case name: `error`, `warn`,
/// `info` or `debug`. Any other text gives an empty optional.
[[nodiscard]] std::optional<spdlog::level::level_enum>
parseSelectableLogLevel(std::string_view aName);

/// The selectable name of a level: `trace` shows as `debug`, `critical` and `off` as `error`.
[[nodiscard]] std::string_view selectableLogLevelName(spdlog::level::level_enum aLevel);

/// Sets the level of the logger that the program writes to (thread-safe).
void setLogLevel(spdlog::level::level_enum aLevel);

/// The level of the logger that the program writes to.
[[nodiscard]] spdlog::level::level_enum currentLogLevel();

/// The log file: `TBR_LOG_FILE` when it is set, else `taxbroker.log` in the state folder
/// (`stateFolder`). Empty when there is no `TBR_LOG_FILE` and no absolute `HOME`.
[[nodiscard]] std::optional<std::filesystem::path> logFilePath(const EnvLookup& aEnv);

[[nodiscard]] spdlog::sink_ptr openLogFile(const EnvLookup& aEnv, bool aTruncate);

/// A file sink that starts a new file at `aMaxBytes` and keeps `aMaxFiles` older ones
/// (`taxbroker.1.log` is the newest of them).
[[nodiscard]] spdlog::sink_ptr
makeLogFileSink(const std::filesystem::path& aFile, std::size_t aMaxBytes, std::size_t aMaxFiles);

/// The names of the `TBR_LOG_*` environment variables that are set but not valid, so the logger
/// ignores them and uses the default. Names only, never the values. `InitializeLogger` writes one
/// warning for each.
[[nodiscard]] std::vector<std::string> InvalidLogSettings();

} // namespace taxbroker

#define LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARNING(...) SPDLOG_WARN(__VA_ARGS__)
#define LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)