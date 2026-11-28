#include "server/browser_open.hpp"

#include "../support/test_environment.hpp"

#include "taxbroker/environment.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#if defined(__linux__)
#include <sys/stat.h>
#endif

namespace {
using namespace taxbroker;

/// Sets PATH for one test and restores it at the end.
class PathOverride {
  public:
    explicit PathOverride(const std::string& aValue) {
        const std::optional<std::string> old = readEnvironment("PATH");
        mHadOld = old.has_value();
        mOld = mHadOld ? *old : "";
        test::setTestEnvironment("PATH", aValue);
    }

    PathOverride(const PathOverride&) = delete;
    PathOverride& operator=(const PathOverride&) = delete;

    ~PathOverride() {
        if (mHadOld)
        {
            test::setTestEnvironment("PATH", mOld);
        }
        else
        {
            test::unsetTestEnvironment("PATH");
        }
    }

  private:
    std::string mOld;
    bool mHadOld = false;
};

TEST(BrowserOpenTest, StartsXdgOpenWithTheFileAsItsOnlyArgument) {
#if !defined(__linux__)
    GTEST_SKIP()
        << "Not built yet: the browser is opened with xdg-open on Linux only; the Windows and "
           "macOS ways to open it are not built yet";
#else
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
#endif
}

TEST(BrowserOpenTest, ReturnsFalseWhenXdgOpenDoesNotExist) {
    const PathOverride path("/nonexistent-folder");

    EXPECT_FALSE(openInBrowser("/run/user/1000/taxbroker/launch.html"));
}

} // namespace
