#pragma once

#include <string>

namespace taxbroker {

struct ApplicationResult;

namespace api {

inline constexpr unsigned kApplicationResultSchemaVersion = 2;

/// Produces the application result object that the future HTTP API returns.
[[nodiscard]] std::string serializeApplicationResultJson(const ApplicationResult& aResult,
                                                         int aIndent = -1);

} // namespace api

} // namespace taxbroker
