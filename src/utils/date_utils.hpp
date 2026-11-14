#pragma once

#include "taxbroker/event_metadata.hpp"
#include "taxbroker/text_format.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace taxbroker {

// YYYY-MM-DDTHH:MM:SS[.fffffffff](Z|+HH:MM|-HH:MM), normalized to UTC milliseconds.
// Sub-millisecond precision is truncated.
[[nodiscard]] std::optional<SourceTimestamp> parseSourceTimestamp(std::string_view aValue);

} // namespace taxbroker
