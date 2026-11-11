#include "cli/output_files.hpp"

#include <algorithm>
#include <iterator>
#include <system_error>

namespace taxbroker {
namespace {

/// The name an earlier file is kept under while the new file replaces it.
std::filesystem::path oldPath(const std::filesystem::path& aPath) {
    auto path = aPath;
    path += ".old";

    return path;
}

/// A step that was done: the final file now holds the new file.
struct DoneStep {
    std::filesystem::path mFinal;
    bool mHadOldFile{};
};

/// Puts back the file of an earlier run, or removes the new file when there was none. Returns
/// false when that fails.
bool undo(const DoneStep& aStep) {
    std::error_code error;

    if (aStep.mHadOldFile)
    {
        std::filesystem::rename(oldPath(aStep.mFinal), aStep.mFinal, error);
    }
    else
    {
        std::filesystem::remove(aStep.mFinal, error);
    }

    return !error;
}

} // namespace

std::filesystem::path temporaryPath(const std::filesystem::path& aPath) {
    auto path = aPath;
    path += ".tmp";

    return path;
}

void removeTemporaryFiles(const std::vector<std::filesystem::path>& aPaths) {
    for (const auto& path : aPaths)
    {
        std::error_code error;

        if (std::filesystem::is_regular_file(path, error))
        {
            std::filesystem::remove(path, error);
        }
    }
}

ReplaceResult replaceFiles(const std::vector<FileReplacement>& aReplacements) {
    ReplaceResult result;
    std::vector<DoneStep> done;
    std::error_code error;

    for (const auto& replacement : aReplacements)
    {
        const bool hadOldFile = std::filesystem::is_regular_file(replacement.mFinal, error);

        error.clear();

        if (hadOldFile)
        {
            std::filesystem::rename(replacement.mFinal, oldPath(replacement.mFinal), error);
        }

        if (!error)
        {
            std::filesystem::rename(replacement.mTemporary, replacement.mFinal, error);
        }

        std::error_code restoreError;

        if (error && hadOldFile && !std::filesystem::exists(replacement.mFinal, restoreError))
        {
            // The earlier file was moved but the new one did not take its place.
            std::filesystem::rename(oldPath(replacement.mFinal), replacement.mFinal, restoreError);

            if (restoreError)
            {
                result.mNotRestored.push_back(replacement.mFinal.filename().string());
            }
        }

        if (error)
        {
            break;
        }

        done.push_back({.mFinal = replacement.mFinal, .mHadOldFile = hadOldFile});
    }

    if (!error)
    {
        for (const auto& step : done)
        {
            if (step.mHadOldFile)
            {
                std::filesystem::remove(oldPath(step.mFinal), error);
            }
        }

        // A copy that cannot be removed is only a stray file; the new files are in place.
        result.mReplaced = true;

        return result;
    }

    for (auto step = done.rbegin(); step != done.rend(); ++step)
    {
        if (!undo(*step))
        {
            result.mNotRestored.push_back(step->mFinal.filename().string());
        }
    }

    std::vector<std::filesystem::path> temporaries;

    std::transform(aReplacements.begin(),
                   aReplacements.end(),
                   std::back_inserter(temporaries),
                   [](const FileReplacement& aReplacement) { return aReplacement.mTemporary; });
    removeTemporaryFiles(temporaries);

    return result;
}

} // namespace taxbroker
