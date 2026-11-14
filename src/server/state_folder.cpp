#include "state_folder.hpp"

#include <stdexcept>
#include <system_error>

namespace taxbroker {

namespace {

/// The value of a variable when it is an absolute path.
std::optional<std::filesystem::path> absolutePath(const EnvLookup& aEnv, std::string_view aName) {
    const std::optional<std::string> value = aEnv(aName);

    if (!value.has_value())
    {
        return std::nullopt;
    }

    const std::filesystem::path path(*value);

    if (!path.is_absolute())
    {
        return std::nullopt;
    }

    return path;
}

} // namespace

std::filesystem::path stateFolder(const EnvLookup& aEnv) {
    // The XDG rule: a relative XDG_STATE_HOME is ignored.
    const std::optional<std::filesystem::path> stateHome = absolutePath(aEnv, "XDG_STATE_HOME");

    if (stateHome.has_value())
    {
        return *stateHome / "taxbroker";
    }

    const std::optional<std::filesystem::path> home = absolutePath(aEnv, "HOME");

    if (!home.has_value())
    {
        throw std::runtime_error("cannot find the state folder: HOME is not an absolute path");
    }

    return *home / ".local" / "state" / "taxbroker";
}

std::filesystem::path launchFilePath(const ServerConfig& aConfig, const EnvLookup& aEnv) {
    if (aConfig.mLaunchFile.has_value())
    {
        return *aConfig.mLaunchFile;
    }

    const std::optional<std::filesystem::path> runtimeDir = absolutePath(aEnv, "XDG_RUNTIME_DIR");

    if (runtimeDir.has_value())
    {
        return *runtimeDir / "taxbroker" / "launch.html";
    }

    return stateFolder(aEnv) / "launch.html";
}

void makePrivateFolder(const std::filesystem::path& aFolder) {
    std::error_code error;

    const bool isLink =
        std::filesystem::is_symlink(std::filesystem::symlink_status(aFolder, error));

    // The messages never name the folder: it holds the user's name.
    if (error && error != std::errc::no_such_file_or_directory)
    {
        throw std::runtime_error("cannot check the private folder");
    }

    if (isLink)
    {
        throw std::runtime_error("the private folder is a symbolic link");
    }

    std::filesystem::create_directories(aFolder, error);

    if (error)
    {
        throw std::runtime_error("cannot create the private folder");
    }

    std::filesystem::permissions(aFolder,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,
                                 error);

    if (error)
    {
        throw std::runtime_error("cannot set the mode of the private folder");
    }
}

} // namespace taxbroker
