#pragma once

#include "server_config.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace taxbroker {

/// The saved taxpayer profile file. It only moves text to and from the disk.
class ProfileStore {
  public:
    /// No file means reads find nothing, removes do nothing and writes throw.
    ProfileStore(std::optional<std::filesystem::path> aFile, std::optional<std::string> aLocation);

    /// `profile.json` in the user's data folder. No store file when `HOME` is not absolute.
    [[nodiscard]] static ProfileStore forUser(const EnvLookup& aEnv);

    [[nodiscard]] const std::optional<std::string>& location() const;

    /// Reads at most aMaxBytes. Empty when there is no file. Throws without the path if unreadable.
    [[nodiscard]] std::optional<std::string> read(std::size_t aMaxBytes) const;

    /// Writes atomically with mode 0600 (folder 0700). Throws without the path; refuses symlinks.
    void write(std::string_view aContent) const;

    /// Deletes the file. No file is fine. Throws without the path on failure.
    void remove() const;

  private:
    std::optional<std::filesystem::path> mFile; ///< Empty when there is no data folder.
    std::optional<std::string> mLocation;       ///< `~/...`, or empty.
};

} // namespace taxbroker
