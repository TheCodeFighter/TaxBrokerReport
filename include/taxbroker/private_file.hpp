#pragma once

// Windows only. Linux and macOS write the private files with POSIX calls (mode 0600, no link).
// For the backend's own programs (profile store, launch file); not part of the application
// service. It is public only because the server may include nothing but include/.

#if defined(_WIN32)

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace taxbroker {

/// What went wrong in a private file call; the caller chooses its own message.
enum class PrivateFileProblem {
    SymbolicLink, ///< The file name is a link or a junction.
    NotRegular,   ///< The opened file is a folder, a link, a pipe or a device.
    Open,         ///< The file cannot be opened.
    Read,         ///< The file cannot be read.
    Write,        ///< The file cannot be written or put in place.
    Delete,       ///< The file cannot be deleted.
};

/// The error of a private file call. The text never names the path.
class PrivateFileError : public std::runtime_error {
  public:
    PrivateFileError(PrivateFileProblem aProblem, const char* aText)
        : std::runtime_error(aText), mProblem(aProblem) {}

    [[nodiscard]] PrivateFileProblem problem() const {
        return mProblem;
    }

  private:
    PrivateFileProblem mProblem; ///< Always set by the constructor.
};

/// Writes the content to `<file>.tmp` for the current user only, flushes it, then puts it in place
/// of the file. A link or a junction at the file name is `SymbolicLink`; a folder there, or a link,
/// a junction or a folder at the `.tmp` name, is `Write`. A stale `.tmp` file is deleted first.
/// The old file stays when the write fails.
void writePrivateFile(const std::filesystem::path& aFile, std::string_view aContent);

/// At most aMaxBytes of the file. Nothing when it does not exist. The opened handle must be a
/// regular file, not a link, a folder, a pipe or a device.
[[nodiscard]] std::optional<std::string> readPrivateFile(const std::filesystem::path& aFile,
                                                         std::size_t aMaxBytes);

/// Deletes the file (a link is deleted, not its target). A missing file is fine.
void removePrivateFile(const std::filesystem::path& aFile);

} // namespace taxbroker

#endif
