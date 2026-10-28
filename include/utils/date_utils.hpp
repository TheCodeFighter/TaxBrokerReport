#pragma once

#include "taxbroker/event_metadata.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace taxbroker {

[[nodiscard]] std::optional<Date> parseCalendarDate(std::string_view aValue);

/// The date as YYYY-MM-DD, for a message parameter.
[[nodiscard]] std::string formatCalendarDate(Date aDate);

// YYYY-MM-DDTHH:MM:SS[.fffffffff](Z|+HH:MM|-HH:MM), normalized to UTC milliseconds.
// Sub-millisecond precision is truncated.
[[nodiscard]] std::optional<SourceTimestamp> parseSourceTimestamp(std::string_view aValue);

} // namespace taxbroker
