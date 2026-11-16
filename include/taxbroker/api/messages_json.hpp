#pragma once

#include <string>

namespace taxbroker::api {

/// The message texts for the frontend, as one JSON object built once per process from the
/// embedded table: `messages` (every message key with `en`, `sl` and its `params` of name and
/// kind) and `terms` (every term id with `en` and `sl`).
[[nodiscard]] const std::string& messagesJson();

} // namespace taxbroker::api
