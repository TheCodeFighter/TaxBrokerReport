#pragma once

#include "http_message.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace taxbroker {

/// The route `/api/log-level`: `GET` answers the level of the log, `POST` with
/// `{"level":"info"}` sets it for the rest of the server session (`error`, `warn`, `info` or
/// `debug`). The guard is done before. Another path below `/api/log-level/` is 404.
[[nodiscard]] HttpResponse handleLogLevelRoute(const HttpRequest& aRequest, std::string_view aPath);

/// The text of a file when it has at most `aMaxBytes` bytes; else its last `aMaxBytes` bytes
/// without the first, cut line. A missing or unreadable file gives an empty text.
[[nodiscard]] std::string readLogTail(const std::filesystem::path& aFile, std::size_t aMaxBytes);

/// The route `/api/log`: `GET` answers the log as one text file, the older file `<name>.1.log`
/// first and then the current file `aLogFile`, each up to its last 5 MiB. No file sink or no
/// text is 404 `no_log_file`. The guard is done before.
[[nodiscard]] HttpResponse handleLogFileRoute(const HttpRequest& aRequest,
                                              const std::optional<std::filesystem::path>& aLogFile);

} // namespace taxbroker
