#pragma once

#include "taxbroker/event_metadata.hpp"

#include <optional>
#include <string_view>

namespace taxbroker {

[[nodiscard]] std::optional<Date> parseCalendarDate(std::string_view aValue);

// YYYY-MM-DDTHH:MM:SS[.fffffffff](Z|+HH:MM|-HH:MM), normalized to UTC milliseconds.
// Sub-millisecond precision is truncated.
[[nodiscard]] std::optional<SourceTimestamp> parseSourceTimestamp(std::string_view aValue);

} // namespace taxbroker
