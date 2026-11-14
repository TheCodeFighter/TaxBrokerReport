#pragma once

#include <filesystem>
#include <functional>

namespace taxbroker {

/// Opens a file in the default browser; true when that could be started. The tests pass their own.
using BrowserOpener = std::function<bool(const std::filesystem::path&)>;

/// Linux: starts `xdg-open <aFile>` without a shell and without a terminal. True when the program
/// started; it may still fail later. A thread reaps the child, as `xdg-open` may run as long as
/// the browser. Other systems: false until their opener exists.
[[nodiscard]] bool openInBrowser(const std::filesystem::path& aFile);

} // namespace taxbroker
