#include "profile_store.hpp"

#include "state_folder.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace taxbroker {

namespace {

constexpr std::string_view kFileName = "profile.json";

/// The file path as `~/...` when it is under `HOME`, else nothing (also for a `HOME` of `/`).
std::optional<std::string> locationText(const std::filesystem::path& aFile, const EnvLookup& aEnv) {
    const std::optional<std::string> home = aEnv("HOME");

    // A home folder of "/" (a container user without a home) would make "~" mean the whole disk.
    if (!home.has_value() || !std::filesystem::path(*home).is_absolute() ||
        std::filesystem::path(*home) == std::filesystem::path(*home).root_path())
    {
        return std::nullopt;
    }

    const std::filesystem::path relative = aFile.lexically_relative(*home);

    if (relative.empty() || *relative.begin() == "..")
    {
        return std::nullopt;
    }

    return "~/" + relative.generic_string();
}

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

} // namespace taxbroker
