#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace taxbroker {

/// Looks up an environment variable. The tests pass a map instead of the real environment.
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

/// The lookup of the real process environment.
[[nodiscard]] EnvLookup systemEnvironment();

/// The value of a variable when it is an absolute path; a relative or missing value is ignored.
[[nodiscard]] std::optional<std::filesystem::path> absoluteEnvironmentPath(const EnvLookup& aEnv,
                                                                           std::string_view aName);

/// The per-user state folder on Linux: `$XDG_STATE_HOME/taxbroker` when that is an absolute path,
/// else `$HOME/.local/state/taxbroker`. Throws `std::runtime_error` when `HOME` is not absolute.
[[nodiscard]] std::filesystem::path stateFolder(const EnvLookup& aEnv);

/// Creates the folder with its parents and sets its mode to `0700`, also when it existed. A
/// symbolic link is refused. Throws `std::runtime_error` without the path in the text.
void makePrivateFolder(const std::filesystem::path& aFolder);

} // namespace taxbroker
