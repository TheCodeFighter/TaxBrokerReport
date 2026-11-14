#include "taxbroker/server/launch_file.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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
        : mPath(std::filesystem::temp_directory_path() / ("tbr_launch_" + aName)) {
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

/// The whole content of a file.
std::string readAll(const std::filesystem::path& aPath) {
    std::ifstream file(aPath, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();

    return content.str();
}

const std::string kToken =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"; // 64 hex characters

TEST(LaunchFileTest, LaunchPageRedirectsToTheLinkWithTheToken) {
    const std::string expected =
        "<!doctype html>\n"
        "<html><head><meta charset=\"utf-8\"><meta name=\"referrer\" content=\"no-referrer\">\n"
        "<meta http-equiv=\"refresh\" content=\"0;url=http://127.0.0.1:8080/#token=" +
        kToken +
        "\">\n"
        "<title>TaxBrokerReport</title></head>\n"
        "<body><a href=\"http://127.0.0.1:8080/#token=" +
        kToken + "\">TaxBrokerReport</a></body></html>\n";

    EXPECT_EQ(makeLaunchPage(8080, kToken), expected);
}

TEST(LaunchFileTest, LaunchPageRejectsATokenThatIsNotHex) {
    // 8 characters of markup and 56 times "a": 64 characters, but not hex.
    const std::string token = "<script>" + std::string(56, 'a');

    EXPECT_THROW((void)makeLaunchPage(8080, token), std::invalid_argument);
}

TEST(LaunchFileTest, LaunchPageRejectsAShortToken) {
    EXPECT_THROW((void)makeLaunchPage(8080, "ab12"), std::invalid_argument);
}

TEST(LaunchFileTest, LaunchFileInAMissingFolderFails) {
    const TempFolder folder("LaunchFileInAMissingFolderFails");

    const std::filesystem::path path = folder.path() / "missing" / "launch.html";

    try
    {
        writeLaunchFile(path, "page");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& error)
    { EXPECT_STREQ(error.what(), "cannot write the launch file"); }
}

#if !defined(_WIN32)

/// The permission bits of a file (for example 0600).
mode_t modeOf(const std::filesystem::path& aPath) {
    struct stat info{};
    EXPECT_EQ(::stat(aPath.c_str(), &info), 0);

    return info.st_mode & 0777;
}

TEST(LaunchFileTest, LaunchFileIsReadableByTheUserOnly) {
    const TempFolder folder("LaunchFileIsReadableByTheUserOnly");

    const std::filesystem::path path = folder.path() / "launch.html";

    writeLaunchFile(path, "page");

    EXPECT_EQ(readAll(path), "page");
    EXPECT_EQ(modeOf(path), static_cast<mode_t>(0600));
}

TEST(LaunchFileTest, ExistingLaunchFileIsReplacedAndMadePrivate) {
    const TempFolder folder("ExistingLaunchFileIsReplacedAndMadePrivate");

    const std::filesystem::path path = folder.path() / "launch.html";

    {
        std::ofstream file(path, std::ios::binary);
        file << "old content";
    }

    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read | std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace);

    writeLaunchFile(path, "page");

    EXPECT_EQ(readAll(path), "page");
    EXPECT_EQ(modeOf(path), static_cast<mode_t>(0600));
}

TEST(LaunchFileTest, LaunchFileIsNotWrittenThroughASymbolicLink) {
    const TempFolder folder("LaunchFileIsNotWrittenThroughASymbolicLink");

    const std::filesystem::path target = folder.path() / "target.txt";
    const std::filesystem::path link = folder.path() / "launch.html";

    {
        std::ofstream file(target, std::ios::binary);
        file << "target";
    }

    std::filesystem::create_symlink(target, link);

    EXPECT_THROW(writeLaunchFile(link, "page"), std::runtime_error);
    EXPECT_EQ(readAll(target), "target");
}

#endif

} // namespace
