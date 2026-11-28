#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace taxbroker {

/// A copy of an environment variable as UTF-8 text, or nothing when it is not set (on Windows also
/// when it is empty or not valid UTF-16). The program never changes its own environment.
[[nodiscard]] std::optional<std::string> readEnvironment(std::string_view aName);

/// A path from UTF-8 text. The bytes are kept as they are on Linux and macOS; on Windows the text
/// is turned into wide characters, so a name with a letter like `č` stays the same folder.
[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view aText);

} // namespace taxbroker
