#pragma once

#include "taxbroker/application_result.hpp"

#include <string>
#include <string_view>

namespace taxbroker {

namespace api {

inline constexpr unsigned kApplicationResultSchemaVersion = 2;

[[nodiscard]] std::string_view diagnosticCodeName(const ApplicationDiagnosticCode& aCode);

/// The request type as in the JSON: `loss_eligibility_confirmation_required`.
[[nodiscard]] std::string_view requestTypeName(RequestType aType);

/// Produces the application result object that the future HTTP API returns.
[[nodiscard]] std::string serializeApplicationResultJson(const ApplicationResult& aResult,
                                                         int aIndent = -1);

} // namespace api

} // namespace taxbroker
