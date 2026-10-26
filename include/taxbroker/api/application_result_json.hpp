#pragma once

#include "taxbroker/application_result.hpp"

#include <string>
#include <string_view>

namespace taxbroker {

namespace api {

inline constexpr unsigned kApplicationResultSchemaVersion = 2;

[[nodiscard]] std::string_view diagnosticCodeName(const ApplicationDiagnosticCode& aCode);

/// Produces the application result object that the future HTTP API returns.
[[nodiscard]] std::string serializeApplicationResultJson(const ApplicationResult& aResult,
                                                         int aIndent = -1);

} // namespace api

} // namespace taxbroker
