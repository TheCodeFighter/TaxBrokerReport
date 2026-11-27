#pragma once

#include "service/ordered_tasks.hpp"
#include "taxbroker/application_request.hpp"
#include "taxbroker/types.hpp"

#include <cstddef>

namespace taxbroker {

/// The result of a file whose parser stopped on an error it does not handle (a programming
/// error): no events and one file-level error, so the other files are still used.
[[nodiscard]] ParseResult parseFailureResult(const BrokerInputFile& aFile);

/// The parse result of a task outcome: its value, or, for a task that threw, the failure result
/// of the file and an error log line with the kind of exception (no path, no row).
[[nodiscard]] ParseResult parseResultOfOutcome(TaskOutcome<ParseResult> aOutcome,
                                               const BrokerInputFile& aFile,
                                               std::size_t aSourceIndex);

} // namespace taxbroker
