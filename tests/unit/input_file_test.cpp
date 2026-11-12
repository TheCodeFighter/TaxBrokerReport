#include "taxbroker/input_file.hpp"

#include "../support/large_allocation_hook.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>

namespace {

using namespace taxbroker;

/// A path under the system temp directory that no other test of this run uses.
std::filesystem::path uniquePath(const std::string& aName) {
    static std::atomic<unsigned> counter{0};

    return std::filesystem::temp_directory_path() /
           ("taxbroker_input_file_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(counter++) + "_" + aName);
}

void writeFile(const std::filesystem::path& aPath, const std::string& aContent) {
    std::ofstream output{aPath, std::ios::binary | std::ios::trunc};

    output << aContent;
}

void removeFile(const std::filesystem::path& aPath) {
    std::error_code ignored;

    std::filesystem::remove_all(aPath, ignored);
}

TEST(InputFileTest, ReadsTheBytesAndTheFileNameOfAFile) {
    const auto path = uniquePath("rates.csv");

    writeFile(path, "a,b\n1,2\n");

    const auto read = readInputFile(path);

    removeFile(path);

    ASSERT_TRUE(std::holds_alternative<InputFile>(read));
    EXPECT_EQ(std::get<InputFile>(read).mContent, "a,b\n1,2\n");
    EXPECT_EQ(std::get<InputFile>(read).mName, path.filename().string());
}

TEST(InputFileTest, ReadsAnEmptyFileAsEmptyContent) {
    const auto path = uniquePath("empty.csv");

    writeFile(path, "");

    const auto read = readInputFile(path);

    removeFile(path);

    ASSERT_TRUE(std::holds_alternative<InputFile>(read));
    EXPECT_TRUE(std::get<InputFile>(read).mContent.empty());
}

TEST(InputFileTest, AMissingFileIsUnreadable) {
    const auto read = readInputFile(uniquePath("missing.csv"));

    ASSERT_TRUE(std::holds_alternative<InputFileFault>(read));
    EXPECT_EQ(std::get<InputFileFault>(read), InputFileFault::Unreadable);
}

TEST(InputFileTest, AFolderIsUnreadable) {
    const auto path = uniquePath("folder.csv");

    std::filesystem::create_directories(path);

    const auto read = readInputFile(path);

    removeFile(path);

    ASSERT_TRUE(std::holds_alternative<InputFileFault>(read));
    EXPECT_EQ(std::get<InputFileFault>(read), InputFileFault::Unreadable);
}

TEST(InputFileTest, AnEmptyPathIsUnreadable) {
    const auto read = readInputFile(std::filesystem::path{});

    ASSERT_TRUE(std::holds_alternative<InputFileFault>(read));
    EXPECT_EQ(std::get<InputFileFault>(read), InputFileFault::Unreadable);
}

TEST(InputFileTest, AFileOverTheLimitIsTooLargeAndAFileOfTheLimitIsRead) {
    const auto path = uniquePath("limit.csv");

    writeFile(path, "0123456789");

    const auto atLimit = readInputFile(path, 10);

    ASSERT_TRUE(std::holds_alternative<InputFile>(atLimit));
    EXPECT_EQ(std::get<InputFile>(atLimit).mContent, "0123456789");

    const auto overLimit = readInputFile(path, 9);

    removeFile(path);

    ASSERT_TRUE(std::holds_alternative<InputFileFault>(overLimit));
    EXPECT_EQ(std::get<InputFileFault>(overLimit), InputFileFault::TooLarge);
}

TEST(InputFileTest, TheRealLimitIs100Megabytes) {
    EXPECT_EQ(kMaxInputFileBytes, 104'857'600U);
    EXPECT_EQ(inputFileLimitMegabytes(kMaxInputFileBytes), 100U);
    EXPECT_EQ(inputFileLimitMegabytes(1'000), 0U);
}

TEST(InputFileTest, RunningOutOfMemoryIsAFault) {
    const auto path = uniquePath("memory.csv");

    writeFile(path, std::string(test::kLargeAllocationBytes, 'x'));

    std::variant<InputFile, InputFileFault> read;

    {
        const test::ScopedAllocationFailure failure;

        read = readInputFile(path);
    }

    removeFile(path);

    ASSERT_TRUE(std::holds_alternative<InputFileFault>(read));
    EXPECT_EQ(std::get<InputFileFault>(read), InputFileFault::OutOfMemory);
}

TEST(InputFileTest, AFileThatBecomesShorterWhileItIsReadIsUnreadable) {
    const auto path = uniquePath("shrinking.csv");

    writeFile(path, std::string(test::kLargeAllocationBytes, 'x'));

    std::variant<InputFile, InputFileFault> read;

    {
        // The memory for the bytes is taken after the size was taken and before they are read;
        // the file is cut to 4 bytes there.
        const test::ScopedLargeAllocationHook hook{
            [&path] { std::filesystem::resize_file(path, 4); }};

        read = readInputFile(path);
    }

    removeFile(path);

    ASSERT_TRUE(std::holds_alternative<InputFileFault>(read));
    EXPECT_EQ(std::get<InputFileFault>(read), InputFileFault::Unreadable);
}

TEST(InputFileTest, AnUnreadableFaultHasNoSharedMessage) {
    EXPECT_THROW((void)inputFileFaultMessage(InputFileFault::Unreadable, "test", 0),
                 std::logic_error);
    EXPECT_THROW((void)inputFileFaultDiagnostic(InputFileFault::Unreadable, "a.csv", "test", 0),
                 std::logic_error);
}

TEST(InputFileTest, TheMessagesOfTheTwoSharedFaultsHaveTheirKeysAndCodes) {
    const auto tooLarge = inputFileFaultDiagnostic(InputFileFault::TooLarge, "a.csv", "test", 4);
    const auto outOfMemory =
        inputFileFaultDiagnostic(InputFileFault::OutOfMemory, "b.csv", "test", 5);

    EXPECT_EQ(tooLarge.mCode, DiagnosticCode::InputFileTooLarge);
    EXPECT_EQ(tooLarge.mMessageKey, MessageKey::InputFileTooLarge);
    EXPECT_EQ(tooLarge.mSourceFile, "a.csv");
    EXPECT_EQ(tooLarge.mMessageParams, (std::vector<MessageParam>{numberParam("limitMb", 100)}));
    EXPECT_EQ(outOfMemory.mCode, DiagnosticCode::InputFileOutOfMemory);
    EXPECT_EQ(outOfMemory.mMessageKey, MessageKey::InputFileOutOfMemory);
    EXPECT_EQ(outOfMemory.mSourceFile, "b.csv");
    EXPECT_TRUE(outOfMemory.mMessageParams.empty());
}

} // namespace
