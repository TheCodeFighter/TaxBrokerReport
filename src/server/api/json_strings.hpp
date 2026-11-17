#pragma once

#include "taxbroker/application_result.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/types.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace taxbroker::api::detail {

/// The API values that the parser JSON and the application JSON share.
[[nodiscard]] std::string_view toString(Broker aBroker);
[[nodiscard]] std::string_view toString(DiagnosticSeverity aSeverity);
[[nodiscard]] std::string_view toString(DiagnosticCode aCode);
[[nodiscard]] std::string_view toString(PipelineStage aStage);
[[nodiscard]] std::string_view toString(ReportType aType);
[[nodiscard]] std::string_view toString(MergeDiagnosticCode aCode);
[[nodiscard]] std::string_view toString(ProcessingDiagnosticCode aCode);
[[nodiscard]] std::string_view toString(RequestType aType);
[[nodiscard]] std::string_view toString(ReportStatus aStatus);
[[nodiscard]] std::string_view toString(OverallStatus aStatus);
[[nodiscard]] std::string_view toString(NoDataReason aReason);
[[nodiscard]] std::string_view toString(FailureResolution aResolution);

/// The key of a report in the application JSON: `capitalGains`, `dividends`, `bankInterest` or
/// `otherInterest`.
[[nodiscard]] std::string_view reportKey(ReportType aType);

/// The stable name of the code of any stage, for example `incomplete_history`.
[[nodiscard]] std::string_view codeString(const ApplicationDiagnosticCode& aCode);

/// A date as `2024-03-05`.
[[nodiscard]] std::string isoDate(const Date& aDate);

/// The parameters as one JSON object: numbers stay numbers, text and term ids are strings.
[[nodiscard]] nlohmann::json messageParamsToJson(const std::vector<MessageParam>& aParams);

} // namespace taxbroker::api::detail
