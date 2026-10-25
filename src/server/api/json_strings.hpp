#pragma once

#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/types.hpp"

#include <nlohmann/json.hpp>

#include <string_view>
#include <vector>

namespace taxbroker::api::detail {

/// The API values that the parser JSON and the application JSON share.
[[nodiscard]] std::string_view toString(Broker aBroker);
[[nodiscard]] std::string_view toString(DiagnosticSeverity aSeverity);
[[nodiscard]] std::string_view toString(DiagnosticCode aCode);

/// The parameters as one JSON object: numbers stay numbers, text and term ids are strings.
[[nodiscard]] nlohmann::json messageParamsToJson(const std::vector<MessageParam>& aParams);

} // namespace taxbroker::api::detail
