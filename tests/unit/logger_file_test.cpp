#include "taxbroker/logging.hpp"

#include <gtest/gtest.h>

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {
using namespace taxbroker;

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

/// A fresh temp folder that is removed at the end of the test.
class TempFolder {
  public:
    explicit TempFolder(const std::string& aName)
        : mPath(std::filesystem::temp_directory_path() / ("tbr_logger_" + aName)) {
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

TEST(LoggerLevelTest, TheDefaultLevelIsError) {
    EXPECT_EQ(defaultLogLevel(), spdlog::level::err);
}

TEST(LoggerLevelTest, OnlyTheFourExactNamesAreSelectable) {
    EXPECT_EQ(parseSelectableLogLevel("error"), spdlog::level::err);
    EXPECT_EQ(parseSelectableLogLevel("warn"), spdlog::level::warn);
    EXPECT_EQ(parseSelectableLogLevel("info"), spdlog::level::info);
    EXPECT_EQ(parseSelectableLogLevel("debug"), spdlog::level::debug);

    EXPECT_FALSE(parseSelectableLogLevel("loud").has_value());
    EXPECT_FALSE(parseSelectableLogLevel("Info").has_value()); // lower case only
    EXPECT_FALSE(parseSelectableLogLevel("warning").has_value());
    EXPECT_FALSE(parseSelectableLogLevel("trace").has_value());
    EXPECT_FALSE(parseSelectableLogLevel("").has_value());
}

TEST(LoggerLevelTest, EveryLevelHasASelectableName) {
    EXPECT_EQ(selectableLogLevelName(spdlog::level::trace), "debug");
    EXPECT_EQ(selectableLogLevelName(spdlog::level::debug), "debug");
    EXPECT_EQ(selectableLogLevelName(spdlog::level::info), "info");
    EXPECT_EQ(selectableLogLevelName(spdlog::level::warn), "warn");
    EXPECT_EQ(selectableLogLevelName(spdlog::level::err), "error");
    EXPECT_EQ(selectableLogLevelName(spdlog::level::critical), "error");
    EXPECT_EQ(selectableLogLevelName(spdlog::level::off), "error");
}

TEST(LoggerFileTest, TheFileIsInTheStateHomeWhenItIsAbsolute) {
#if defined(_WIN32)
    GTEST_SKIP()
        << "Not built yet: the Windows state folder is not built yet: the test names a Linux path "
           "(/h, /s), which is not absolute on Windows";
#else
    const auto file = logFilePath(envOf({{"XDG_STATE_HOME", "/s"}, {"HOME", "/h"}}));

    EXPECT_EQ(file, std::filesystem::path{"/s/taxbroker/taxbroker.log"});
#endif
}

TEST(LoggerFileTest, TheFileIsInTheHomeStateFolderWithoutAStateHome) {
#if defined(_WIN32)
    GTEST_SKIP()
        << "Not built yet: the Windows state folder is not built yet: the test names a Linux path "
           "(/h, /s), which is not absolute on Windows";
#else
    const auto file = logFilePath(envOf({{"HOME", "/h"}}));

    EXPECT_EQ(file, std::filesystem::path{"/h/.local/state/taxbroker/taxbroker.log"});
#endif
}

TEST(LoggerFileTest, ARelativeStateHomeIsIgnored) {
#if defined(_WIN32)
    GTEST_SKIP()
        << "Not built yet: the Windows state folder is not built yet: the test names a Linux path "
           "(/h, /s), which is not absolute on Windows";
#else
    const auto file = logFilePath(envOf({{"XDG_STATE_HOME", "rel"}, {"HOME", "/h"}}));

    EXPECT_EQ(file, std::filesystem::path{"/h/.local/state/taxbroker/taxbroker.log"});
#endif
}

TEST(LoggerFileTest, WithoutAnAbsoluteHomeThereIsNoFile) {
    EXPECT_FALSE(logFilePath(envOf({})).has_value());
    EXPECT_FALSE(logFilePath(envOf({{"HOME", "relative"}})).has_value());
}

TEST(LoggerFileTest, TheLogFileVariableWins) {
    const auto file = logFilePath(envOf({{"TBR_LOG_FILE", "/f.log"}, {"HOME", "/h"}}));

    EXPECT_EQ(file, std::filesystem::path{"/f.log"});
}

TEST(LoggerFileTest, TheDefaultFileIsOpenedInAPrivateStateFolder) {
    const TempFolder temp("default_file");
    const auto sink = openLogFile(envOf({{"XDG_STATE_HOME", temp.path().string()}}), false);

    EXPECT_NE(sink, nullptr);
    EXPECT_TRUE(std::filesystem::exists(temp.path() / "taxbroker" / "taxbroker.log"));

#if !defined(_WIN32)
    // On Windows the access list of the folder is checked in windows_private_file_test.cpp.
    struct stat info{};
    ASSERT_EQ(::stat((temp.path() / "taxbroker").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777U, 0700U); // owner only
#endif
}

TEST(LoggerFileTest, ALogFileVariableIsOpenedWithoutMakingTheStateFolder) {
    const TempFolder temp("explicit_file");
    const auto file = temp.path() / "taxbroker_test.log";
    const auto sink = openLogFile(envOf({{"TBR_LOG_FILE", file.string()},
                                         {"XDG_STATE_HOME", (temp.path() / "state").string()}}),
                                  false);

    EXPECT_NE(sink, nullptr);
    EXPECT_TRUE(std::filesystem::exists(file));
    EXPECT_FALSE(std::filesystem::exists(temp.path() / "state"));
}

TEST(LoggerFileTest, TruncateEmptiesTheFileAndAppendKeepsIt) {
    const TempFolder temp("truncate_file");
    const auto file = temp.path() / "taxbroker_test.log";
    const EnvLookup environment = envOf({{"TBR_LOG_FILE", file.string()}});

    std::ofstream(file) << "old"; // 3 bytes

    EXPECT_NE(openLogFile(environment, false), nullptr);
    EXPECT_EQ(std::filesystem::file_size(file), 3U);

    EXPECT_NE(openLogFile(environment, true), nullptr);
    EXPECT_EQ(std::filesystem::file_size(file), 0U);
}

TEST(LoggerFileTest, WithoutAFilePathThereIsNoSink) {
    EXPECT_EQ(openLogFile(envOf({}), false), nullptr);
}

TEST(LoggerFileTest, AFileThatCannotBeOpenedGivesNoSink) {
    const TempFolder temp("blocked_file");
    const auto blocker = temp.path() / "a_file";

    std::ofstream(blocker) << "x";

    // The folder of the log file is a regular file, so the log file cannot be created.
    EXPECT_EQ(openLogFile(envOf({{"TBR_LOG_FILE", (blocker / "taxbroker.log").string()}}), false),
              nullptr);
}

TEST(LoggerFileTest, TheProductionFilesRotateAtFiveMiBAndKeepThree) {
    EXPECT_EQ(kLogFileMaxBytes, 5U * 1024U * 1024U); // 5 MiB = 5'242'880 bytes
    EXPECT_EQ(kLogFileCount, 3U);
}

TEST(LoggerFileTest, TheFileRotatesAtTheLimitAndKeepsTheLastFiles) {
    const auto folder = std::filesystem::temp_directory_path() / "tbr_logger_rotation_test";

    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    {
        // 100 lines of about 70 bytes are about 7 KB: seven times the 1 KiB limit, so more
        // than three files are needed and the oldest ones are dropped.
        spdlog::logger logger("rotation_test", makeLogFileSink(folder / "taxbroker.log", 1024, 3));
        const std::string line(60, 'x');

        for (int count = 0; count < 100; ++count)
        {
            logger.error(line);
        }

        logger.flush();
    }

    EXPECT_TRUE(std::filesystem::exists(folder / "taxbroker.log"));
    EXPECT_TRUE(std::filesystem::exists(folder / "taxbroker.1.log"));
    EXPECT_TRUE(std::filesystem::exists(folder / "taxbroker.2.log"));
    EXPECT_TRUE(std::filesystem::exists(folder / "taxbroker.3.log"));
    EXPECT_FALSE(std::filesystem::exists(folder / "taxbroker.4.log"));

    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

} // namespace
