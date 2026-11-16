#pragma once

#include "server_config.hpp"

#include <filesystem>

namespace taxbroker {

/// The per-user state folder on Linux: `$XDG_STATE_HOME/taxbroker` when that is an absolute path,
/// else `$HOME/.local/state/taxbroker`. Throws `std::runtime_error` when `HOME` is not absolute.
[[nodiscard]] std::filesystem::path stateFolder(const EnvLookup& aEnv);

/// The per-user data folder on Linux: `$XDG_DATA_HOME/taxbroker` when that is an absolute path,
/// else `$HOME/.local/share/taxbroker`. Throws `std::runtime_error` when `HOME` is not absolute.
[[nodiscard]] std::filesystem::path dataFolder(const EnvLookup& aEnv);

/// The launch file: `TBR_LAUNCH_FILE` when set; else `$XDG_RUNTIME_DIR/taxbroker/launch.html` for
/// an absolute `XDG_RUNTIME_DIR` (a snap browser reads it there); else in the state folder.
[[nodiscard]] std::filesystem::path launchFilePath(const ServerConfig& aConfig,
                                                   const EnvLookup& aEnv);

/// Creates the folder with its parents and sets its mode to `0700`, also when it existed. A
/// symbolic link is refused. Throws `std::runtime_error` without the path in the text.
void makePrivateFolder(const std::filesystem::path& aFolder);

} // namespace taxbroker
