#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace taxbroker {

/// The number of characters of a text that XML accepts: valid UTF-8 without control characters
/// (below U+0020) and without U+FFFE and U+FFFF. Returns nothing for any other text.
[[nodiscard]] std::optional<std::size_t> xmlTextLength(std::string_view aText);

} // namespace taxbroker
