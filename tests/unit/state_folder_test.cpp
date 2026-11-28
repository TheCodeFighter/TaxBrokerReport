#include "server/state_folder.hpp"

#include "../support/test_environment.hpp"

#include "taxbroker/environment.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {
using namespace taxbroker;

/// A fresh, empty folder for one test; removed before and after.
class TempFolder {
  public:
    explicit TempFolder(const std::string& aName)
        : mPath(std::filesystem::temp_directory_path() / ("tbr_state_" + aName)) {
        std::filesystem::remove_all(mPath);
        std::filesystem::create_directories(mPath);
    }

    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(mPath, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return mPath;
    }

  private:
    std::filesystem::path mPath; ///< Always set by the constructor.
};

/// An environment made of a map, so the test names every variable it sets.
EnvLookup envOf(std::map<std::string, std::string> aValues) {
    return [aValues](std::string_view aName) -> std::optional<std::string> {
        const auto found = aValues.find(std::string(aName));

        if (found == aValues.end())
        {
            return std::nullopt;
        }

        return found->second;
    };
}

#if !defined(_WIN32)

/// The permission bits (rwx of owner, group, others) of a file.
unsigned modeOf(const std::filesystem::path& aPath) {
    struct stat info{};
    EXPECT_EQ(::stat(aPath.c_str(), &info), 0);

    return info.st_mode & 0777U;
}

#endif

/// The message of the std::runtime_error an action throws; a fixed text when it throws nothing.
std::string messageOfThrow(const std::function<void()>& aAction) {
    try
    { aAction(); } catch (const std::runtime_error& error)
    { return error.what(); }

    return "no exception";
}

ServerConfig configWithoutLaunchFile() {
    ServerConfig config;
    config.mHost = "127.0.0.1";
    config.mPort = 0;

    return config;
}

TEST(StateFolderTest, AbsoluteStateHomeIsUsed) {
    const TempFolder temp("state_home");
    const std::string stateHome = (temp.path() / "state").string();

    EXPECT_EQ(stateFolder(envOf({{"XDG_STATE_HOME", stateHome}, {"HOME", "/home/someone"}})),
              temp.path() / "state" / "taxbroker");
}

TEST(StateFolderTest, RelativeStateHomeIsIgnoredAndHomeIsUsed) {
    const TempFolder temp("relative_state_home");

    // XDG says a relative XDG_STATE_HOME is ignored.
    EXPECT_EQ(
        stateFolder(envOf({{"XDG_STATE_HOME", "relative/state"}, {"HOME", temp.path().string()}})),
        temp.path() / ".local" / "state" / "taxbroker");
}

TEST(StateFolderTest, MissingHomeThrowsWithoutAValue) {
    EXPECT_EQ(messageOfThrow([&]() { (void)stateFolder(envOf({})); }),
              "cannot find the state folder: HOME is not an absolute path");
}

TEST(StateFolderTest, AbsoluteDataHomeIsUsed) {
#if defined(_WIN32)
    GTEST_SKIP()
        << "Not built yet: the Windows state folder is not built yet: the test names a Linux path "
           "(/data, /home/someone), which is not absolute on Windows";
#else
    EXPECT_EQ(dataFolder(envOf({{"XDG_DATA_HOME", "/data"}, {"HOME", "/home/someone"}})),
              std::filesystem::path("/data/taxbroker"));
#endif
}

TEST(StateFolderTest, RelativeDataHomeIsIgnoredAndHomeIsUsed) {
#if defined(_WIN32)
    GTEST_SKIP()
        << "Not built yet: the Windows state folder is not built yet: the test names a Linux path "
           "(/home/someone), which is not absolute on Windows";
#else
    // XDG says a relative XDG_DATA_HOME is ignored.
    EXPECT_EQ(dataFolder(envOf({{"XDG_DATA_HOME", "relative/data"}, {"HOME", "/home/someone"}})),
              std::filesystem::path("/home/someone/.local/share/taxbroker"));
#endif
}

TEST(StateFolderTest, MissingHomeThrowsWithoutAValueForTheDataFolder) {
    EXPECT_EQ(messageOfThrow([&]() { (void)dataFolder(envOf({})); }),
              "cannot find the data folder: HOME is not an absolute path");
}

TEST(StateFolderTest, LaunchFileFromTheConfigWins) {
    ServerConfig config = configWithoutLaunchFile();
    config.mLaunchFile = std::filesystem::path("/workspace/runtime/launch/launch.html");

    EXPECT_EQ(
        launchFilePath(config, envOf({{"XDG_RUNTIME_DIR", "/run/user/1000"}, {"HOME", "/home/a"}})),
        std::filesystem::path("/workspace/runtime/launch/launch.html"));
}

TEST(StateFolderTest, LaunchFileIsInTheRuntimeFolderWhenThereIsOne) {
#if defined(_WIN32)
    GTEST_SKIP()
        << "Not built yet: the Windows state folder is not built yet: the test names a Linux path "
           "(/run/user/1000), which is not absolute on Windows";
#else
    EXPECT_EQ(launchFilePath(configWithoutLaunchFile(),
                             envOf({{"XDG_RUNTIME_DIR", "/run/user/1000"}, {"HOME", "/home/a"}})),
              std::filesystem::path("/run/user/1000/taxbroker/launch.html"));
#endif
}

TEST(StateFolderTest, LaunchFileIsInTheStateFolderWithoutARuntimeFolder) {
#if defined(_WIN32)
    GTEST_SKIP()
        << "Not built yet: the Windows state folder is not built yet: the test names a Linux path "
           "(/home/a), which is not absolute on Windows";
#else
    EXPECT_EQ(launchFilePath(configWithoutLaunchFile(), envOf({{"HOME", "/home/a"}})),
              std::filesystem::path("/home/a/.local/state/taxbroker/launch.html"));
#endif
}

TEST(StateFolderTest, RelativeRuntimeFolderIsIgnored) {
#if defined(_WIN32)
    GTEST_SKIP()
        << "Not built yet: the Windows state folder is not built yet: the test names a Linux path "
           "(/home/a), which is not absolute on Windows";
#else
    EXPECT_EQ(launchFilePath(configWithoutLaunchFile(),
                             envOf({{"XDG_RUNTIME_DIR", "run"}, {"HOME", "/home/a"}})),
              std::filesystem::path("/home/a/.local/state/taxbroker/launch.html"));
#endif
}

TEST(StateFolderTest, NewPrivateFolderIsMadeWithItsParents) {
    const TempFolder temp("new_folder");
    const std::filesystem::path folder = temp.path() / "a" / "taxbroker";

    makePrivateFolder(folder);

    EXPECT_TRUE(std::filesystem::is_directory(folder));
}

#if !defined(_WIN32)

TEST(StateFolderTest, NewPrivateFolderHasModeSevenHundred) {
    const TempFolder temp("new_folder_mode");
    const std::filesystem::path folder = temp.path() / "a" / "taxbroker";

    makePrivateFolder(folder);

    EXPECT_EQ(modeOf(folder), 0700U);
}

TEST(StateFolderTest, ExistingFolderGetsModeSevenHundred) {
    const TempFolder temp("existing_folder");
    const std::filesystem::path folder = temp.path() / "taxbroker";
    std::filesystem::create_directories(folder);
    std::filesystem::permissions(folder, static_cast<std::filesystem::perms>(0755));

    makePrivateFolder(folder);

    EXPECT_EQ(modeOf(folder), 0700U);
}

#endif

TEST(StateFolderTest, SymbolicLinkIsRefusedWithoutThePathInTheMessage) {
    const TempFolder temp("link_folder");
    const std::filesystem::path target = temp.path() / "target";
    const std::filesystem::path link = temp.path() / "link";
    std::filesystem::create_directories(target);
    std::filesystem::create_directory_symlink(target, link);

    EXPECT_EQ(messageOfThrow([&]() { makePrivateFolder(link); }),
              "the private folder is a symbolic link");
}

TEST(StateFolderTest, FolderThatIsAFileIsRefusedWithoutThePathInTheMessage) {
    const TempFolder temp("file_folder");
    const std::filesystem::path file = temp.path() / "taxbroker";
    std::ofstream(file) << "not a folder";

    EXPECT_EQ(messageOfThrow([&]() { makePrivateFolder(file); }),
              "cannot create the private folder");
}

#if !defined(_WIN32)

TEST(StateFolderTest, FolderBelowAFileIsRefusedWithoutThePathInTheMessage) {
    const TempFolder temp("below_file");
    const std::filesystem::path file = temp.path() / "taxbroker";
    std::ofstream(file) << "not a folder";

    // The parent is a file, so even looking at the folder fails (ENOTDIR).
    EXPECT_EQ(messageOfThrow([&]() { makePrivateFolder(file / "inner"); }),
              "cannot check the private folder");
}

#endif

TEST(StateFolderTest, FolderWhoseModeCannotBeChangedIsRefusedWithoutThePathInTheMessage) {
#if !defined(__linux__)
    GTEST_SKIP() << "procfs, the folder that refuses every mode change, exists on Linux only";
#else
    // procfs never allows chmod, for any user, so the mode of this existing folder cannot be set.
    EXPECT_EQ(messageOfThrow([&]() { makePrivateFolder("/proc/1"); }),
              "cannot set the mode of the private folder");
#endif
}

TEST(EnvironmentTest, ASetVariableIsRead) {
    test::setTestEnvironment("TBR_TEST_VALUE", "abc");

    EXPECT_EQ(readEnvironment("TBR_TEST_VALUE"), "abc");

    test::unsetTestEnvironment("TBR_TEST_VALUE");
}

TEST(EnvironmentTest, AVariableThatIsNotSetGivesNothing) {
    test::unsetTestEnvironment("TBR_TEST_VALUE");

    EXPECT_EQ(readEnvironment("TBR_TEST_VALUE"), std::nullopt);
}

#if defined(_WIN32)

TEST(EnvironmentTest, AnEmptyVariableIsNotSetOnWindows) {
    // _wputenv_s with an empty value removes the variable, so there is nothing to read.
    test::setTestEnvironment("TBR_TEST_VALUE", "");

    EXPECT_EQ(readEnvironment("TBR_TEST_VALUE"), std::nullopt);
}

#else
TEST(EnvironmentTest, AnEmptyVariableIsReadAsEmptyText) {
    test::setTestEnvironment("TBR_TEST_VALUE", "");

    EXPECT_EQ(readEnvironment("TBR_TEST_VALUE"), "");

    test::unsetTestEnvironment("TBR_TEST_VALUE");
}

#endif

TEST(EnvironmentTest, AValueIsReadAsUtf8) {
    // "\xC4\x8D" is the UTF-8 text of the letter c with a caron.
    test::setTestEnvironment("TBR_TEST_VALUE", "\xC4\x8D");

    EXPECT_EQ(readEnvironment("TBR_TEST_VALUE"), "\xC4\x8D");

    test::unsetTestEnvironment("TBR_TEST_VALUE");
}

TEST(EnvironmentTest, APathFromUtf8KeepsTheLetter) {
    // Two UTF-8 bytes are one character of the path on every platform.
    EXPECT_TRUE(pathFromUtf8("/tmp/\xC4\x8D").u8string() == u8"/tmp/\u010D");
}

#if defined(_WIN32)

TEST(EnvironmentTest, APathFromUtf8IsWideOnWindows) {
    EXPECT_EQ(pathFromUtf8("C:\\\xC4\x8D").native(), L"C:\\\u010D");
}

#endif

} // namespace
