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

/// One log line about a result: the revision, the overall and per-report status and the numbers of
/// requests, errors and warnings, for example `revision=2 status=needs_input
/// capitalGains=needs_input dividends=no_data bankInterest=no_data otherInterest=no_data requests=1
/// errors=0 warnings=2`. Only fixed words and counts: no name, ISIN, amount or file name.
[[nodiscard]] std::string resultLogSummary(const ApplicationResult& aResult);

/// The open question types, error codes, warning codes, exclusion reasons, no-data reasons and
/// answered failure questions of a result with their counts, sorted by name, for example
/// `requests [none]; errors [incomplete_history x1]; warnings [none]; excluded [none]; no data
/// [none]; answered [none]`. Only fixed words and counts.
[[nodiscard]] std::string resultLogDetails(const ApplicationResult& aResult);

/// Writes the result of a step to the log as two info lines, `Run result: <summary>` and
/// `Run details: <details>`. Only fixed words and counts, so the lines can be sent with a bug
/// report.
void logResult(const ApplicationResult& aResult);

/// Produces the application result object that the HTTP API returns.
[[nodiscard]] std::string serializeApplicationResultJson(const ApplicationResult& aResult,
                                                         int aIndent = -1);

} // namespace api

} // namespace taxbroker
