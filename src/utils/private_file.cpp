#include "taxbroker/private_file.hpp"

#if defined(_WIN32)

#include "windows_security.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace taxbroker {

namespace {

/// A pipe at the path must not let the program at its other end act with the user's rights.
constexpr DWORD kOpenFlags =
    FILE_FLAG_OPEN_REPARSE_POINT | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS;

bool isLink(DWORD aAttributes) {
    return (aAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool isLinkOrFolder(DWORD aAttributes) {
    return (aAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0;
}

/// Attributes by name; a failure to look is a write problem.
std::optional<DWORD> attributesForWrite(const std::filesystem::path& aPath) {
    try
    { return windows::attributesOf(aPath); } catch (const std::runtime_error&)
    { throw PrivateFileError(PrivateFileProblem::Write, "cannot write the file"); }
}

/// Writes all bytes. False when a write fails.
bool writeAll(HANDLE aFile, std::string_view aContent) {
    std::size_t written = 0;

    while (written < aContent.size())
    {
        // WriteFile takes a 32-bit count.
        const DWORD wanted = static_cast<DWORD>(
            std::min<std::size_t>(aContent.size() - written, std::numeric_limits<DWORD>::max()));
        DWORD count = 0;

        if (!::WriteFile(aFile, aContent.data() + written, wanted, &count, nullptr) || count == 0)
        {
            return false;
        }

        written += count;
    }

    return true;
}

/// Makes the temporary file for the user only (CREATE_NEW, share mode 0: nobody else has it open),
/// writes the content and flushes it to the disk. The handle is closed when this returns.
bool writeTemporaryFile(const std::filesystem::path& aTemporary,
                        std::string_view aContent,
                        SECURITY_ATTRIBUTES& aAttributes) {
    const windows::Handle file(::CreateFileW(aTemporary.c_str(),
                                             GENERIC_WRITE,
                                             0,
                                             &aAttributes,
                                             CREATE_NEW,
                                             FILE_ATTRIBUTE_NORMAL | kOpenFlags,
                                             nullptr));

    return file.valid() && writeAll(file.get(), aContent) && ::FlushFileBuffers(file.get()) != 0;
}

} // namespace

void writePrivateFile(const std::filesystem::path& aFile, std::string_view aContent) {
    // MoveFileExW replaces a link itself, so a link, a junction or a folder is refused by name.
    // The folder is private, so no other user can swap the name between this check and the move.
    const std::optional<DWORD> fileAttributes = attributesForWrite(aFile);

    if (fileAttributes.has_value() && isLink(*fileAttributes))
    {
        throw PrivateFileError(PrivateFileProblem::SymbolicLink, "the file is a link");
    }

    if (fileAttributes.has_value() && isLinkOrFolder(*fileAttributes))
    {
        throw PrivateFileError(PrivateFileProblem::Write, "cannot write the file");
    }

    // Never path::string(): it throws for a character outside the ANSI code page.
    std::filesystem::path temporary = aFile;
    temporary += L".tmp";

    // A stale temporary file keeps its old access list in CreateFileW, so it is deleted first.
    // A link there is refused and left alone, as O_NOFOLLOW does on Linux.
    const std::optional<DWORD> temporaryAttributes = attributesForWrite(temporary);

    if (temporaryAttributes.has_value())
    {
        if (isLinkOrFolder(*temporaryAttributes))
        {
            throw PrivateFileError(PrivateFileProblem::Write, "cannot write the file");
        }

        if (!::DeleteFileW(temporary.c_str()))
        {
            throw PrivateFileError(PrivateFileProblem::Write, "cannot write the file");
        }
    }

    windows::UserOnlyDescriptor descriptor(false);
    SECURITY_ATTRIBUTES attributes = descriptor.attributes();

    if (!writeTemporaryFile(temporary, aContent, attributes))
    {
        ::DeleteFileW(temporary.c_str());

        throw PrivateFileError(PrivateFileProblem::Write, "cannot write the file");
    }

    // The old file stays when the move fails (for example while a virus scanner holds it open).
    if (!::MoveFileExW(temporary.c_str(),
                       aFile.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        ::DeleteFileW(temporary.c_str());

        throw PrivateFileError(PrivateFileProblem::Write, "cannot write the file");
    }
}

std::optional<std::string> readPrivateFile(const std::filesystem::path& aFile,
                                           std::size_t aMaxBytes) {
    // BACKUP_SEMANTICS lets a folder be opened, so that the handle check below can refuse it.
    const windows::Handle file(::CreateFileW(aFile.c_str(),
                                             GENERIC_READ,
                                             FILE_SHARE_READ | FILE_SHARE_DELETE,
                                             nullptr,
                                             OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS | kOpenFlags,
                                             nullptr));

    if (!file.valid())
    {
        const DWORD error = ::GetLastError();

        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        {
            return std::nullopt;
        }

        throw PrivateFileError(PrivateFileProblem::Open, "cannot open the file");
    }

    // The type first: the attribute call can fail on a pipe, which would give the wrong message.
    // A pipe would block the read, so only a file on a disk is read.
    FILE_ATTRIBUTE_TAG_INFO tag{};

    if (::GetFileType(file.get()) != FILE_TYPE_DISK ||
        !::GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
        isLinkOrFolder(tag.FileAttributes))
    {
        throw PrivateFileError(PrivateFileProblem::NotRegular, "the file is not a regular file");
    }

    std::string content;
    std::array<char, 4096> buffer{};

    while (content.size() < aMaxBytes)
    {
        const DWORD wanted =
            static_cast<DWORD>(std::min<std::size_t>(buffer.size(), aMaxBytes - content.size()));
        DWORD count = 0;

        if (!::ReadFile(file.get(), buffer.data(), wanted, &count, nullptr))
        {
            throw PrivateFileError(PrivateFileProblem::Read, "cannot read the file");
        }

        if (count == 0)
        {
            break;
        }

        content.append(buffer.data(), count);
    }

    return content;
}

void removePrivateFile(const std::filesystem::path& aFile) {
    if (!::DeleteFileW(aFile.c_str()))
    {
        const DWORD error = ::GetLastError();

        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        {
            throw PrivateFileError(PrivateFileProblem::Delete, "cannot delete the file");
        }
    }
}

} // namespace taxbroker

#endif
