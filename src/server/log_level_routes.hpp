#pragma once

#include "http_message.hpp"

#include <string_view>

namespace taxbroker {

/// The route `/api/log-level`: `GET` answers the level of the log, `POST` with
/// `{"level":"info"}` sets it for the rest of the server session (`error`, `warn`, `info` or
/// `debug`). The guard is done before. Another path below `/api/log-level/` is 404.
[[nodiscard]] HttpResponse handleLogLevelRoute(const HttpRequest& aRequest, std::string_view aPath);

} // namespace taxbroker
