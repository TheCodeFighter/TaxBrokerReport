#include "server/browser_open.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <sys/stat.h>

namespace {
using namespace taxbroker;

/// Sets PATH for one test and restores it at the end.
class PathOverride {
  public:
    explicit PathOverride(const std::string& aValue) {
        const char* old = std::getenv("PATH");
        mHadOld = old != nullptr;
        mOld = mHadOld ? old : "";
        ::setenv("PATH", aValue.c_str(), 1);
    }

    PathOverride(const PathOverride&) = delete;
    PathOverride& operator=(const PathOverride&) = delete;

    ~PathOverride() {
        if (mHadOld)
        {
            ::setenv("PATH", mOld.c_str(), 1);
        }
        else
        {
            ::unsetenv("PATH");
        }
    }

  private:
    std::string mOld;
    bool mHadOld = false;
};

TEST(BrowserOpenTest, StartsXdgOpenWithTheFileAsItsOnlyArgument) {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "tbr_browser_open";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    // A fake xdg-open writes its first argument into a pipe, so the test waits without a sleep.
    const std::filesystem::path pipe = folder / "argument.pipe";
    ASSERT_EQ(::mkfifo(pipe.c_str(), 0600), 0);

    const std::filesystem::path script = folder / "xdg-open";
    std::ofstream(script) << "#!/bin/sh\nprintf '%s' \"$1\" > '" << pipe.string() << "'\n";
    std::filesystem::permissions(script, std::filesystem::perms::owner_all);

    // The fake comes first: a real xdg-open would never write to the pipe, and the test would hang.
    const PathOverride path(folder.string() + ":/usr/bin:/bin");

    EXPECT_TRUE(openInBrowser("/run/user/1000/taxbroker/launch.html"));

    std::ifstream reader(pipe);
    std::string argument;
    std::getline(reader, argument);

    EXPECT_EQ(argument, "/run/user/1000/taxbroker/launch.html");

    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

TEST(BrowserOpenTest, ReturnsFalseWhenXdgOpenDoesNotExist) {
    const PathOverride path("/nonexistent-folder");

    EXPECT_FALSE(openInBrowser("/run/user/1000/taxbroker/launch.html"));
}

} // namespace
