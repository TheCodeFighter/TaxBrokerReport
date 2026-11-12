#include "taxbroker/input_file.hpp"

#include "taxbroker/diagnostic_messages.hpp"
#include "utils/logger.hpp"

#include <fstream>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace taxbroker {
std::uint64_t inputFileLimitMegabytes(const std::size_t aMaxBytes) {
    return aMaxBytes / (1024U * 1024U);
}

std::variant<InputFile, InputFileFault> readInputFile(const std::filesystem::path& aPath,
                                                      const std::size_t aMaxBytes) {
    // On some systems a folder opens and fails only while it is read, and opening a named pipe
    // waits for a writer, so only a regular file is opened.
    std::error_code error;
    const bool regularFile = std::filesystem::is_regular_file(aPath, error);
    const std::uintmax_t size = regularFile ? std::filesystem::file_size(aPath, error) : 0;

    if (error || !regularFile)
    {
        return InputFileFault::Unreadable;
    }

    if (size > aMaxBytes)
    {
        return InputFileFault::TooLarge;
    }

    std::ifstream input{aPath, std::ios::binary};

    if (!input)
    {
        return InputFileFault::Unreadable;
    }

    try
    {
        InputFile file{.mName = aPath.filename().string(), .mContent = {}};

        file.mContent.resize(static_cast<std::size_t>(size));
        input.read(file.mContent.data(), static_cast<std::streamsize>(size));

        // A read error, or a file that became shorter after its size was taken, is not a
        // complete file.
        if (static_cast<std::uintmax_t>(input.gcount()) != size)
        {
            return InputFileFault::Unreadable;
        }

        return file;
    } catch (const std::bad_alloc&)
    { return InputFileFault::OutOfMemory; }
}

DiagnosticMessage inputFileFaultMessage(const InputFileFault aFault,
                                        const std::string_view aInputKind,
                                        const std::size_t aSourceIndex,
                                        const std::size_t aMaxBytes) {
    switch (aFault)
    {
    case InputFileFault::TooLarge:
        LOG_ERROR("Input file {} ({}) rejected: input_file_too_large, limit {} MB",
                  aSourceIndex,
                  aInputKind,
                  inputFileLimitMegabytes(aMaxBytes));

        return makeDiagnosticMessage(MessageKey::InputFileTooLarge,
                                     {numberParam("limitMb", inputFileLimitMegabytes(aMaxBytes))});
    case InputFileFault::OutOfMemory:
        LOG_ERROR("Input file {} ({}) rejected: input_file_out_of_memory",
                  aSourceIndex,
                  aInputKind);

        return makeDiagnosticMessage(MessageKey::InputFileOutOfMemory);
    case InputFileFault::Unreadable:
        break;
    }

    throw std::logic_error{"An unreadable file is worded by each loader"};
}

ParseDiagnostic inputFileFaultDiagnostic(const InputFileFault aFault,
                                         std::string aFileName,
                                         const std::string_view aInputKind,
                                         const std::size_t aSourceIndex,
                                         const std::size_t aMaxBytes) {
    auto message = inputFileFaultMessage(aFault, aInputKind, aSourceIndex, aMaxBytes);

    return ParseDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = aFault == InputFileFault::TooLarge ? DiagnosticCode::InputFileTooLarge
                                                    : DiagnosticCode::InputFileOutOfMemory,
        .mSourceFile = std::move(aFileName),
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    };
}

} // namespace taxbroker
