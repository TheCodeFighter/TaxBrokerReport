#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace taxbroker {

/// A new file written under a temporary name, and the path it replaces.
struct FileReplacement {
    std::filesystem::path mTemporary;
    std::filesystem::path mFinal;
};

/// What replaceFiles did.
struct ReplaceResult {
    bool mReplaced{};                      ///< True when every file was replaced.
    std::vector<std::string> mNotRestored; ///< Names of earlier files that could not be restored.
};

/// The name an output file is written under before it replaces the file of an earlier run.
[[nodiscard]] std::filesystem::path temporaryPath(const std::filesystem::path& aPath);

/// Deletes the temporary files of a failed run. Something that is not a file is left alone: this
/// run did not make it.
void removeTemporaryFiles(const std::vector<std::filesystem::path>& aPaths);

/**
 * Replaces each final file with its temporary file, in order. An earlier file is first moved to
 * `<name>.old`. When a step fails, the steps already done are undone in reverse order, so the
 * files of an earlier run are as they were, and every temporary file is deleted. On success the
 * `.old` copies are deleted.
 */
[[nodiscard]] ReplaceResult replaceFiles(const std::vector<FileReplacement>& aReplacements);

} // namespace taxbroker
