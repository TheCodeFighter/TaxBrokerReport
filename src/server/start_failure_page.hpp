#pragma once

#include "taxbroker/state_folder.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace taxbroker {

/// The page that tells a user who started the program without a terminal that it did not start:
/// fixed HTML without script, Slovenian first and then English. It holds the error line
/// (HTML-escaped), the version, what to do and the ways to report it. Paths never appear.
[[nodiscard]] std::string makeStartFailurePage(std::string_view aError, std::string_view aVersion);

/// Writes the page as `start_failed.html` readable by the user only: in
/// `$XDG_RUNTIME_DIR/taxbroker/` for an absolute `XDG_RUNTIME_DIR`, else in the state folder.
/// Returns its path, or nothing when no folder can be made or the file cannot be written (a
/// symbolic link at its place is refused).
[[nodiscard]] std::optional<std::filesystem::path>
writeStartFailurePage(const EnvLookup& aEnv, std::string_view aError, std::string_view aVersion);

/// Deletes an old `start_failed.html` from the same folder; nothing happens when there is none.
void removeStartFailurePage(const EnvLookup& aEnv);

} // namespace taxbroker
