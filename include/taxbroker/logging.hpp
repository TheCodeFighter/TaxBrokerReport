#pragma once

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace taxbroker {

void InitializeLogger();

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