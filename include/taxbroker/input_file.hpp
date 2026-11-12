#pragma once

#include "taxbroker/diagnostics.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>

namespace taxbroker {

/// The largest user file the app reads, in bytes (100 MB). A bigger file is a wrong file.
inline constexpr std::size_t kMaxInputFileBytes = 100U * 1024U * 1024U;

/// One user file held in memory: its display name and its bytes.
struct InputFile {
    std::string mName;    ///< The display file name only.
    std::string mContent; ///< The bytes of the file.
};

/// Why a user file gave no text.
enum class InputFileFault {
    Unreadable,
    TooLarge,
    OutOfMemory,
};

/// The size limit in whole megabytes, for the message of `InputFileFault::TooLarge`.
[[nodiscard]] std::uint64_t inputFileLimitMegabytes(std::size_t aMaxBytes);

/// Reads a file into memory. The name is the last part of the path (empty for an empty path).
/// A file over aMaxBytes is not read. Never throws for a bad path, size or memory. The app always
/// uses the default limit; a smaller one lets a test use a small file.
[[nodiscard]] std::variant<InputFile, InputFileFault>
readInputFile(const std::filesystem::path& aPath, std::size_t aMaxBytes = kMaxInputFileBytes);

/// The message of a `TooLarge` or `OutOfMemory` fault, and its error log line (kind, source index
/// and code; no path or name; the limit for `TooLarge`). Throws std::logic_error for
/// `Unreadable`, which each loader words itself.
[[nodiscard]] DiagnosticMessage inputFileFaultMessage(InputFileFault aFault,
                                                      std::string_view aInputKind,
                                                      std::size_t aSourceIndex,
                                                      std::size_t aMaxBytes = kMaxInputFileBytes);

/// The error diagnostic of a `TooLarge` or `OutOfMemory` fault of one file (file name only), with
/// the message and the log line of `inputFileFaultMessage`.
[[nodiscard]] ParseDiagnostic inputFileFaultDiagnostic(InputFileFault aFault,
                                                       std::string aFileName,
                                                       std::string_view aInputKind,
                                                       std::size_t aSourceIndex,
                                                       std::size_t aMaxBytes = kMaxInputFileBytes);

} // namespace taxbroker
