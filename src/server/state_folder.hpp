#pragma once

#include "server_config.hpp"

#include "taxbroker/state_folder.hpp"

#include <filesystem>

namespace taxbroker {

/// The per-user data folder on Linux: `$XDG_DATA_HOME/taxbroker` when that is an absolute path,
/// else `$HOME/.local/share/taxbroker`. Throws `std::runtime_error` when `HOME` is not absolute.
[[nodiscard]] std::filesystem::path dataFolder(const EnvLookup& aEnv);

/// The launch file: `TBR_LAUNCH_FILE` when set; else `$XDG_RUNTIME_DIR/taxbroker/launch.html` for
/// an absolute `XDG_RUNTIME_DIR` (a snap browser reads it there); else in the state folder.
[[nodiscard]] std::filesystem::path launchFilePath(const ServerConfig& aConfig,
                                                   const EnvLookup& aEnv);

} // namespace taxbroker
