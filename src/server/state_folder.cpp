#include "state_folder.hpp"

#include <stdexcept>
#include <system_error>

namespace taxbroker {

std::filesystem::path dataFolder(const EnvLookup& aEnv) {
    // The XDG rule: a relative XDG_DATA_HOME is ignored.
    const std::optional<std::filesystem::path> dataHome =
        absoluteEnvironmentPath(aEnv, "XDG_DATA_HOME");

    if (dataHome.has_value())
    {
        return *dataHome / "taxbroker";
    }

    const std::optional<std::filesystem::path> home = absoluteEnvironmentPath(aEnv, "HOME");

    if (!home.has_value())
    {
        throw std::runtime_error("cannot find the data folder: HOME is not an absolute path");
    }

    return *home / ".local" / "share" / "taxbroker";
}

std::filesystem::path launchFilePath(const ServerConfig& aConfig, const EnvLookup& aEnv) {
    if (aConfig.mLaunchFile.has_value())
    {
        return *aConfig.mLaunchFile;
    }

    const std::optional<std::filesystem::path> runtimeDir =
        absoluteEnvironmentPath(aEnv, "XDG_RUNTIME_DIR");

    if (runtimeDir.has_value())
    {
        return *runtimeDir / "taxbroker" / "launch.html";
    }

    return stateFolder(aEnv) / "launch.html";
}

} // namespace taxbroker
