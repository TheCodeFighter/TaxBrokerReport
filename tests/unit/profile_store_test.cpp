#include "server/profile_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
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
        : mPath(std::filesystem::temp_directory_path() / ("tbr_profile_" + aName)) {
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

std::string readAll(const std::filesystem::path& aPath) {
    std::ifstream file(aPath, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();

    return content.str();
}

#if !defined(_WIN32)

/// The permission bits of a file or folder, for example 0600.
unsigned modeOf(const std::filesystem::path& aPath) {
    struct stat info{};

    EXPECT_EQ(::stat(aPath.c_str(), &info), 0);

    return info.st_mode & 07777U;
}

#endif

/// The text of the exception that the call throws, or "no exception".
std::string messageOfThrow(const std::function<void()>& aCall) {
    try
    { aCall(); } catch (const std::runtime_error& error)
    { return error.what(); }

    return "no exception";
}

TEST(ProfileStoreTest, ReadingWithoutAFileGivesNothing) {
    const TempFolder temp("no_file");
    const ProfileStore store(temp.path() / "data" / "profile.json", std::nullopt);

    EXPECT_EQ(store.read(100), std::nullopt);
}

TEST(ProfileStoreTest, ReadGivesTheContentAndCutsItAtTheLimit) {
    const TempFolder temp("read_limit");
    const ProfileStore store(temp.path() / "profile.json", std::nullopt);

    std::ofstream(temp.path() / "profile.json") << "0123456789";

    EXPECT_EQ(store.read(100), "0123456789");
    EXPECT_EQ(store.read(4), "0123");
}

TEST(ProfileStoreTest, WriteMakesAPrivateFolderAndAPrivateFileWithTheExactContent) {
    const TempFolder temp("write");
    const ProfileStore store(temp.path() / "data" / "taxbroker" / "profile.json", std::nullopt);
    const std::string content = R"({"isResident":true,"name":"Ana Novak","taxNumber":"12345679"})"
                                "\n";

    store.write(content);

    EXPECT_EQ(readAll(temp.path() / "data" / "taxbroker" / "profile.json"), content);

#if !defined(_WIN32)
    // On Windows the access lists are checked in windows_private_file_test.cpp.
    EXPECT_EQ(modeOf(temp.path() / "data" / "taxbroker"), 0700U);
    EXPECT_EQ(modeOf(temp.path() / "data" / "taxbroker" / "profile.json"), 0600U);
#endif
}

TEST(ProfileStoreTest, WriteLeavesNoTemporaryFile) {
    const TempFolder temp("no_temporary");
    const ProfileStore store(temp.path() / "taxbroker" / "profile.json", std::nullopt);

    store.write("{}\n");

    EXPECT_FALSE(std::filesystem::exists(temp.path() / "taxbroker" / "profile.json.tmp"));
}

TEST(ProfileStoreTest, WriteReplacesAnOldFile) {
    const TempFolder temp("replace_content");
    const std::filesystem::path file = temp.path() / "profile.json";
    const ProfileStore store(file, std::nullopt);

    store.write(R"({"a":1})");
    store.write(R"({"a":2})");

    EXPECT_EQ(readAll(file), R"({"a":2})");
}

#if !defined(_WIN32)

TEST(ProfileStoreTest, WriteReplacesAnOldFileAndMakesItPrivate) {
    const TempFolder temp("replace");
    const std::filesystem::path file = temp.path() / "profile.json";
    const ProfileStore store(file, std::nullopt);

    std::ofstream(file) << "old content";
    ASSERT_EQ(::chmod(file.c_str(), 0644), 0);

    store.write("new content");

    EXPECT_EQ(readAll(file), "new content");
    EXPECT_EQ(modeOf(file), 0600U);
}

#endif

TEST(ProfileStoreTest, WriteRefusesASymbolicLinkAndLeavesItsTarget) {
    const TempFolder temp("write_link");
    const std::filesystem::path target = temp.path() / "target.txt";
    const std::filesystem::path file = temp.path() / "profile.json";
    const ProfileStore store(file, std::nullopt);

    std::ofstream(target) << "target content";
    std::filesystem::create_symlink(target, file);

    EXPECT_EQ(messageOfThrow([&]() { store.write("new content"); }),
              "the profile file is a symbolic link");
    EXPECT_EQ(readAll(target), "target content");
    EXPECT_TRUE(std::filesystem::is_symlink(file));
}

TEST(ProfileStoreTest, WriteThrowsWithoutThePathWhenTheFolderCannotBeMade) {
    const TempFolder temp("no_folder");

    // "data" is a file, so "data/profile.json" cannot be made.
    std::ofstream(temp.path() / "data") << "a file";

    const ProfileStore store(temp.path() / "data" / "profile.json", std::nullopt);

    EXPECT_EQ(messageOfThrow([&]() { store.write("x"); }), "cannot create the private folder");
}

TEST(ProfileStoreTest, WriteThrowsWhenTheTemporaryFileCannotBeMade) {
    const TempFolder temp("temporary_blocked");

    // A folder named like the temporary file: it cannot be opened for writing.
    std::filesystem::create_directories(temp.path() / "profile.json.tmp");

    const ProfileStore store(temp.path() / "profile.json", std::nullopt);

    EXPECT_EQ(messageOfThrow([&]() { store.write("x"); }), "cannot write the profile file");
}

TEST(ProfileStoreTest, WriteThrowsAndRemovesTheTemporaryFileWhenTheFileCannotBeReplaced) {
    const TempFolder temp("replace_blocked");

    // A folder with a file in it has the name of the profile: the rename cannot replace it.
    std::filesystem::create_directories(temp.path() / "profile.json");
    std::ofstream(temp.path() / "profile.json" / "inside.txt") << "x";

    const ProfileStore store(temp.path() / "profile.json", std::nullopt);

    EXPECT_EQ(messageOfThrow([&]() { store.write("x"); }), "cannot write the profile file");
    EXPECT_FALSE(std::filesystem::exists(temp.path() / "profile.json.tmp"));
    EXPECT_TRUE(std::filesystem::exists(temp.path() / "profile.json" / "inside.txt"));
}

#if !defined(_WIN32)

TEST(ProfileStoreTest, ReadRefusesASymbolicLink) {
    const TempFolder temp("read_link");
    const std::filesystem::path target = temp.path() / "target.txt";
    const ProfileStore store(temp.path() / "profile.json", std::nullopt);

    std::ofstream(target) << "target content";
    std::filesystem::create_symlink(target, temp.path() / "profile.json");

    EXPECT_EQ(messageOfThrow([&]() { (void)store.read(100); }), "cannot open the profile file");
}

#endif

TEST(ProfileStoreTest, ReadRefusesAFolder) {
    const TempFolder temp("read_folder");
    const ProfileStore store(temp.path() / "profile.json", std::nullopt);

    std::filesystem::create_directories(temp.path() / "profile.json");

    EXPECT_EQ(messageOfThrow([&]() { (void)store.read(100); }),
              "the profile file is not a regular file");
}

TEST(ProfileStoreTest, RemoveDeletesTheFileAndAFileThatIsNotThereIsFine) {
    const TempFolder temp("remove");
    const std::filesystem::path file = temp.path() / "profile.json";
    const ProfileStore store(file, std::nullopt);

    std::ofstream(file) << "content";

    store.remove();

    EXPECT_FALSE(std::filesystem::exists(file));

    EXPECT_EQ(messageOfThrow([&]() { store.remove(); }), "no exception");
}

TEST(ProfileStoreTest, RemoveThrowsWithoutThePathWhenTheNameIsAFolder) {
    const TempFolder temp("remove_folder");
    const ProfileStore store(temp.path() / "profile.json", std::nullopt);

    std::filesystem::create_directories(temp.path() / "profile.json");

    EXPECT_EQ(messageOfThrow([&]() { store.remove(); }), "cannot delete the profile file");
}

TEST(ProfileStoreTest, TheStoreOfTheUserIsInTheDataFolder) {
    const TempFolder temp("for_user");
    const ProfileStore store = ProfileStore::forUser(
        envOf({{"XDG_DATA_HOME", (temp.path() / "data").string()}, {"HOME", "/home/u"}}));

    store.write("{}\n");

    EXPECT_EQ(readAll(temp.path() / "data" / "taxbroker" / "profile.json"), "{}\n");
}

TEST(ProfileStoreTest, TheLocationOfAFileOutsideHomeIsLeftOut) {
    const ProfileStore store =
        ProfileStore::forUser(envOf({{"XDG_DATA_HOME", "/data"}, {"HOME", "/home/u"}}));

    EXPECT_EQ(store.location(), std::nullopt);
}

TEST(ProfileStoreTest, TheLocationOfTheDefaultFolderStartsWithATilde) {
#if defined(_WIN32)
    GTEST_SKIP()
        << "Not built yet: the Windows data folder is not built yet: the test names a Linux path "
           "(/home/u), which is not absolute on Windows";
#endif

    // A relative XDG_DATA_HOME is ignored.
    const ProfileStore store =
        ProfileStore::forUser(envOf({{"XDG_DATA_HOME", "data"}, {"HOME", "/home/u"}}));

    EXPECT_EQ(store.location(), "~/.local/share/taxbroker/profile.json");
}

TEST(ProfileStoreTest, TheLocationOfADataFolderUnderHomeStartsWithATilde) {
#if defined(_WIN32)
    GTEST_SKIP()
        << "Not built yet: the Windows data folder is not built yet: the test names a Linux path "
           "(/home/u), which is not absolute on Windows";
#endif

    const ProfileStore store =
        ProfileStore::forUser(envOf({{"XDG_DATA_HOME", "/home/u/data"}, {"HOME", "/home/u"}}));

    EXPECT_EQ(store.location(), "~/data/taxbroker/profile.json");
}

TEST(ProfileStoreTest, TheLocationIsLeftOutWhenHomeIsNotAbsolute) {
    const ProfileStore store =
        ProfileStore::forUser(envOf({{"XDG_DATA_HOME", "/data"}, {"HOME", "home/u"}}));

    EXPECT_EQ(store.location(), std::nullopt);
}

TEST(ProfileStoreTest, TheLocationIsLeftOutWhenHomeIsTheRootFolder) {
    // A container user without a home has HOME=/; "~" would then name the whole disk.
    const ProfileStore store =
        ProfileStore::forUser(envOf({{"XDG_DATA_HOME", "/workspace/runtime/data"}, {"HOME", "/"}}));

    EXPECT_EQ(store.location(), std::nullopt);
}

TEST(ProfileStoreTest, WithoutAnyFolderTheStoreOfTheUserHasNoFileAndNoLocation) {
    // No HOME and no XDG_DATA_HOME: the server must still start, so the store only has no file.
    const ProfileStore store = ProfileStore::forUser(envOf({}));

    EXPECT_EQ(store.location(), std::nullopt);
    EXPECT_EQ(store.read(100), std::nullopt);
    EXPECT_EQ(messageOfThrow([&]() { store.write("{}\n"); }),
              "there is no data folder for the profile file");
    EXPECT_EQ(messageOfThrow([&]() { store.remove(); }), "no exception");
}

} // namespace
