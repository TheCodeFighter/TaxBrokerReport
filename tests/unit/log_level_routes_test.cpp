#include "../support/log_capture.hpp"
#include "taxbroker/logging.hpp"
#include "server/log_level_routes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace {
using namespace taxbroker;

constexpr std::string_view kPath = "/api/log-level";

HttpResponse get(std::string_view aPath = kPath) {
    return handleLogLevelRoute(HttpRequest{.mMethod = "GET", .mTarget = std::string{aPath}}, aPath);
}

HttpResponse post(const std::string& aBody) {
    return handleLogLevelRoute(
        HttpRequest{.mMethod = "POST", .mTarget = std::string{kPath}, .mBody = aBody},
        kPath);
}

TEST(LogLevelRoutesTest, GetAnswersTheCurrentLevel) {
    LogCapture log;
    setLogLevel(spdlog::level::err);

    const HttpResponse response = get();

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(response.mBody, R"({"level":"error"})");
}

TEST(LogLevelRoutesTest, PostSetsTheLevelAndLogsItOnce) {
    LogCapture log;
    setLogLevel(spdlog::level::err);

    const HttpResponse response = post(R"({"level":"debug"})");

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(response.mBody, R"({"level":"debug"})");
    EXPECT_EQ(currentLogLevel(), spdlog::level::debug);
    EXPECT_EQ(log.count("Log level set: debug"), 1U);
    EXPECT_EQ(get().mBody, R"({"level":"debug"})");
}

TEST(LogLevelRoutesTest, ASetToErrorIsNotShownInTheLogItself) {
    LogCapture log;
    setLogLevel(spdlog::level::info);

    const HttpResponse response = post(R"({"level":"error"})");

    EXPECT_EQ(response.mBody, R"({"level":"error"})");
    EXPECT_EQ(log.count("Log level set"), 0U); // an info line, hidden by the new level
}

TEST(LogLevelRoutesTest, AWrongBodyIsRefusedAndTheLevelStays) {
    LogCapture log;
    setLogLevel(spdlog::level::warn);

    // A level that is not one of the four, a number, no key, an extra key, a body that is not
    // JSON and an empty body.
    for (const std::string body : {R"({"level":"loud"})",
                                   R"({"level":3})",
                                   R"({})",
                                   R"({"level":"info","x":1})",
                                   "not json",
                                   ""})
    {
        const HttpResponse response = post(body);

        EXPECT_EQ(response.mStatus, 400U) << body;
        EXPECT_EQ(response.mBody, R"({"error":"bad_log_level"})") << body;
        EXPECT_EQ(currentLogLevel(), spdlog::level::warn) << body;
    }

    EXPECT_EQ(log.count("Log level request refused: 400 bad_log_level"), 6U);
}

TEST(LogLevelRoutesTest, ABodyOverOneKiBIsRefused) {
    LogCapture log;
    setLogLevel(spdlog::level::warn); // the refusal is a warning

    const HttpResponse response = post(std::string(1025, ' '));

    EXPECT_EQ(response.mStatus, 413U);
    EXPECT_EQ(currentLogLevel(), spdlog::level::warn);
    EXPECT_EQ(log.count("Log level request refused: 413 request_too_large (limit 1024)"), 1U);
}

TEST(LogLevelRoutesTest, AnotherMethodIsRefused) {
    LogCapture log;

    const HttpResponse response =
        handleLogLevelRoute(HttpRequest{.mMethod = "PUT", .mTarget = std::string{kPath}}, kPath);

    EXPECT_EQ(response.mStatus, 405U);
    EXPECT_EQ(log.count("Log level request refused: 405 method_not_allowed"), 1U);
}

TEST(LogLevelRoutesTest, AnotherPathIsNotFound) {
    LogCapture log;

    const HttpResponse response = get("/api/log-level/extra");

    EXPECT_EQ(response.mStatus, 404U);
    EXPECT_EQ(log.count("Log level request refused: 404 not_found"), 1U);
}

/// A temp folder with the log files of one test: `taxbroker.log` and `taxbroker.1.log`.
class LogFileRoutesTest : public ::testing::Test {
  protected:
    LogFileRoutesTest()
        : mFolder(std::filesystem::temp_directory_path() /
                  (std::string("tbr_log_file_routes_") +
                   ::testing::UnitTest::GetInstance()->current_test_info()->name())),
          mCurrent(mFolder / "taxbroker.log"), mOlder(mFolder / "taxbroker.1.log") {}

    void SetUp() override {
        std::filesystem::remove_all(mFolder);
        std::filesystem::create_directories(mFolder);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(mFolder, ignored);
    }

    static void write(const std::filesystem::path& aFile, const std::string& aContent) {
        std::ofstream(aFile, std::ios::binary) << aContent;
    }

    static HttpResponse getLog(const std::optional<std::filesystem::path>& aFile) {
        return handleLogFileRoute(HttpRequest{.mMethod = "GET", .mTarget = "/api/log"}, aFile);
    }

    static std::optional<std::string> headerOf(const HttpResponse& aResponse,
                                               const std::string& aName) {
        const auto found =
            std::find_if(aResponse.mHeaders.begin(),
                         aResponse.mHeaders.end(),
                         [&aName](const HttpField& aField) { return aField.mName == aName; });

        if (found == aResponse.mHeaders.end())
        {
            return std::nullopt;
        }

        return found->mValue;
    }

    std::filesystem::path mFolder;
    std::filesystem::path mCurrent;
    std::filesystem::path mOlder;
};

TEST_F(LogFileRoutesTest, ReadLogTailGivesAWholeFileThatFits) {
    write(mCurrent, "a\nb\n"); // 4 bytes, the limit is 8

    EXPECT_EQ(readLogTail(mCurrent, 8), "a\nb\n");
}

TEST_F(LogFileRoutesTest, ReadLogTailCutsTheFirstLineOfALargerFile) {
    write(mCurrent, "first\nsecond\n"); // 13 bytes, the limit is 8

    // The last 8 bytes are "\nsecond\n"; the cut drops everything up to the first line break.
    EXPECT_EQ(readLogTail(mCurrent, 8), "second\n");
}

TEST_F(LogFileRoutesTest, ReadLogTailOfOnlyTheEndOfALineGivesNothing) {
    write(mCurrent, "abcdefghijkl\n"); // 13 bytes, the limit is 4

    // The last 4 bytes are "jkl\n": the cut line is all there is, so nothing is left.
    EXPECT_EQ(readLogTail(mCurrent, 4), "");
}

TEST_F(LogFileRoutesTest, ReadLogTailWithNoLineBreakInTheLastBytesGivesNothing) {
    write(mCurrent, "abcdefghijkl"); // 12 bytes, no line break at all, the limit is 4

    // The last 4 bytes are "ijkl": no whole line starts in them.
    EXPECT_EQ(readLogTail(mCurrent, 4), "");
}

TEST_F(LogFileRoutesTest, ReadLogTailOfAMissingOrEmptyFileIsEmpty) {
    write(mOlder, "");

    EXPECT_EQ(readLogTail(mCurrent, 8), ""); // there is no such file
    EXPECT_EQ(readLogTail(mOlder, 8), "");   // the file is empty
}

TEST_F(LogFileRoutesTest, GetAnswersTheCurrentFileAsATextDownload) {
    LogCapture log;
    write(mCurrent, "b\n");

    const HttpResponse response = getLog(mCurrent);

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(response.mBody, "b\n");
    EXPECT_EQ(headerOf(response, "Content-Type"), "text/plain; charset=utf-8");
    EXPECT_EQ(headerOf(response, "Content-Disposition"),
              "attachment; filename=\"taxbroker-log.txt\"");
    EXPECT_EQ(log.count("Log downloaded: 2 bytes"), 1U);
}

TEST_F(LogFileRoutesTest, GetGivesTheOlderFileFirst) {
    LogCapture log;
    write(mOlder, "a\n");
    write(mCurrent, "b\n");

    const HttpResponse response = getLog(mCurrent);

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(response.mBody, "a\nb\n");
}

TEST_F(LogFileRoutesTest, GetWorksWithOnlyTheOlderFile) {
    LogCapture log;
    write(mOlder, "a\n");

    const HttpResponse response = getLog(mCurrent);

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(response.mBody, "a\n");
}

TEST_F(LogFileRoutesTest, GetWithoutAFileSinkIsNotFound) {
    LogCapture log;

    const HttpResponse response = getLog(std::nullopt);

    EXPECT_EQ(response.mStatus, 404U);
    EXPECT_EQ(response.mBody, R"({"error":"no_log_file"})");
    EXPECT_EQ(log.count("Log request refused: 404 no_log_file"), 1U);
}

TEST_F(LogFileRoutesTest, GetWithNoFilesOrEmptyFilesIsNotFound) {
    LogCapture log;

    const HttpResponse none = getLog(mCurrent); // neither file exists

    write(mOlder, "");
    write(mCurrent, "");

    const HttpResponse empty = getLog(mCurrent); // both files are empty

    EXPECT_EQ(none.mStatus, 404U);
    EXPECT_EQ(empty.mStatus, 404U);
    EXPECT_EQ(empty.mBody, R"({"error":"no_log_file"})");
    EXPECT_EQ(log.count("Log request refused: 404 no_log_file"), 2U);
}

TEST_F(LogFileRoutesTest, AnotherMethodIsRefused) {
    LogCapture log;
    write(mCurrent, "b\n");

    const HttpResponse response =
        handleLogFileRoute(HttpRequest{.mMethod = "POST", .mTarget = "/api/log"}, mCurrent);

    EXPECT_EQ(response.mStatus, 405U);
    EXPECT_EQ(headerOf(response, "Allow"), "GET");
    EXPECT_EQ(log.count("Log request refused: 405 method_not_allowed"), 1U);
}

} // namespace
