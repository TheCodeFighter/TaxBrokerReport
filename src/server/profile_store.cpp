#include "profile_store.hpp"

#include "state_folder.hpp"

#include "taxbroker/environment.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include "taxbroker/private_file.hpp"
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace taxbroker {

namespace {

constexpr std::string_view kFileName = "profile.json";

/// The file path as `~/...` when it is under `HOME`, else nothing (also for a `HOME` of `/`).
std::optional<std::string> locationText(const std::filesystem::path& aFile, const EnvLookup& aEnv) {
    const std::optional<std::string> home = aEnv("HOME");

    // A home folder of "/" (a container user without a home) would make "~" mean the whole disk.
    const std::filesystem::path homePath =
        home.has_value() ? pathFromUtf8(*home) : std::filesystem::path{};

    if (!home.has_value() || !homePath.is_absolute() || homePath == homePath.root_path())
    {
        return std::nullopt;
    }

    const std::filesystem::path relative = aFile.lexically_relative(homePath);

    if (relative.empty() || *relative.begin() == "..")
    {
        return std::nullopt;
    }

    // The UTF-8 form: generic_string() throws on Windows for a letter outside the ANSI code page.
    const std::u8string text = relative.generic_u8string();

    return "~/" + std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

#if !defined(_WIN32)

/// Writes all bytes to the descriptor. False when the write fails.
bool writeAll(int aDescriptor, std::string_view aContent) {
    std::size_t written = 0;

    while (written < aContent.size())
    {
        const ssize_t count =
            ::write(aDescriptor, aContent.data() + written, aContent.size() - written);

        if (count < 0 && errno == EINTR)
        {
            continue;
        }

        if (count <= 0)
        {
            return false;
        }

        written += static_cast<std::size_t>(count);
    }

    return true;
}

#endif

} // namespace

ProfileStore::ProfileStore(std::optional<std::filesystem::path> aFile,
                           std::optional<std::string> aLocation)
    : mFile(std::move(aFile)), mLocation(std::move(aLocation)) {}

ProfileStore ProfileStore::forUser(const EnvLookup& aEnv) {
    try
    {
        std::filesystem::path file = dataFolder(aEnv) / kFileName;
        std::optional<std::string> location = locationText(file, aEnv);

        return ProfileStore(std::move(file), std::move(location));
    } catch (const std::runtime_error&)
    { return ProfileStore(std::nullopt, std::nullopt); }
}

const std::optional<std::string>& ProfileStore::location() const {
    return mLocation;
}

#if defined(_WIN32)

std::optional<std::string> ProfileStore::read(std::size_t aMaxBytes) const {
    if (!mFile.has_value())
    {
        return std::nullopt;
    }

    try
    { return readPrivateFile(*mFile, aMaxBytes); } catch (const PrivateFileError& error)
    {
        if (error.problem() == PrivateFileProblem::NotRegular)
        {
            throw std::runtime_error("the profile file is not a regular file");
        }

        if (error.problem() == PrivateFileProblem::Read)
        {
            throw std::runtime_error("cannot read the profile file");
        }

        throw std::runtime_error("cannot open the profile file");
    }
}

#else

std::optional<std::string> ProfileStore::read(std::size_t aMaxBytes) const {
    if (!mFile.has_value())
    {
        return std::nullopt;
    }

    const int descriptor = ::open(mFile->c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

    if (descriptor < 0)
    {
        if (errno == ENOENT)
        {
            return std::nullopt;
        }

        throw std::runtime_error("cannot open the profile file");
    }

    struct stat info{};

    // A named pipe would block the read, so only a regular file is read.
    if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode))
    {
        ::close(descriptor);

        throw std::runtime_error("the profile file is not a regular file");
    }

    std::string content;
    std::array<char, 4096> buffer{};

    while (content.size() < aMaxBytes)
    {
        const std::size_t wanted = std::min(buffer.size(), aMaxBytes - content.size());
        const ssize_t count = ::read(descriptor, buffer.data(), wanted);

        if (count < 0 && errno == EINTR)
        {
            continue;
        }

        if (count < 0)
        {
            ::close(descriptor);

            throw std::runtime_error("cannot read the profile file");
        }

        if (count == 0)
        {
            break;
        }

        content.append(buffer.data(), static_cast<std::size_t>(count));
    }

    ::close(descriptor);

    return content;
}

#endif

#if defined(_WIN32)

void ProfileStore::write(std::string_view aContent) const {
    if (!mFile.has_value())
    {
        throw std::runtime_error("there is no data folder for the profile file");
    }

    makePrivateFolder(mFile->parent_path());

    // A link, a junction or a folder at the file name is refused inside writePrivateFile.
    try
    { writePrivateFile(*mFile, aContent); } catch (const PrivateFileError& error)
    {
        if (error.problem() == PrivateFileProblem::SymbolicLink)
        {
            throw std::runtime_error("the profile file is a symbolic link");
        }

        throw std::runtime_error("cannot write the profile file");
    }
}

#else

void ProfileStore::write(std::string_view aContent) const {
    if (!mFile.has_value())
    {
        throw std::runtime_error("there is no data folder for the profile file");
    }

    std::error_code error;

    // rename() would replace a link itself, so a link is refused by name.
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(*mFile, error)))
    {
        throw std::runtime_error("the profile file is a symbolic link");
    }

    makePrivateFolder(mFile->parent_path());

    const std::filesystem::path temporary = mFile->string() + ".tmp";
    const int descriptor =
        ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);

    if (descriptor < 0)
    {
        throw std::runtime_error("cannot write the profile file");
    }

    // An existing temporary file keeps its old mode in open(), so set it again.
    bool succeeded = ::fchmod(descriptor, 0600) == 0 && writeAll(descriptor, aContent) &&
                     ::fsync(descriptor) == 0;

    succeeded = ::close(descriptor) == 0 && succeeded;
    succeeded = succeeded && ::rename(temporary.c_str(), mFile->c_str()) == 0;

    if (!succeeded)
    {
        ::unlink(temporary.c_str());

        throw std::runtime_error("cannot write the profile file");
    }
}

#endif

#if defined(_WIN32)

void ProfileStore::remove() const {
    if (!mFile.has_value())
    {
        return;
    }

    try
    { removePrivateFile(*mFile); } catch (const PrivateFileError&)
    { throw std::runtime_error("cannot delete the profile file"); }
}

#else

void ProfileStore::remove() const {
    if (!mFile.has_value())
    {
        return;
    }

    if (::unlink(mFile->c_str()) != 0 && errno != ENOENT)
    {
        throw std::runtime_error("cannot delete the profile file");
    }
}

#endif

} // namespace taxbroker
