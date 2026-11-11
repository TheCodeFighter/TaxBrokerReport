#include "cli/output_files.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using taxbroker::FileReplacement;
using taxbroker::replaceFiles;

/// A temporary folder for the files of one test.
class TemporaryFolder {
  public:
    TemporaryFolder() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_output_files_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create the synthetic output-files directory"};
        }
    }

    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;

    ~TemporaryFolder() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    [[nodiscard]] std::filesystem::path path(const std::string& aName) const {
        return mDirectory / aName;
    }

    void write(const std::string& aName, const std::string& aText) const {
        std::ofstream output{path(aName), std::ios::binary};

        output << aText;
    }

    [[nodiscard]] std::string read(const std::string& aName) const {
        std::ifstream input{path(aName), std::ios::binary};

        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    /// The names in the folder, sorted.
    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> names;

        for (const auto& entry : std::filesystem::directory_iterator{mDirectory})
        {
            names.push_back(entry.path().filename().string());
        }

        std::sort(names.begin(), names.end());

        return names;
    }

  private:
    std::filesystem::path mDirectory;
};

TEST(OutputFilesTest, ReplacesEveryFileAndLeavesNoOldOrTemporaryFile) {
    const TemporaryFolder folder;

    folder.write("a.xml", "old a");
    folder.write("a.xml.tmp", "new a");
    folder.write("b.xml.tmp", "new b");

    const auto result =
        replaceFiles({{.mTemporary = folder.path("a.xml.tmp"), .mFinal = folder.path("a.xml")},
                      {.mTemporary = folder.path("b.xml.tmp"), .mFinal = folder.path("b.xml")}});

    EXPECT_TRUE(result.mReplaced);
    EXPECT_TRUE(result.mNotRestored.empty());
    EXPECT_EQ(folder.read("a.xml"), "new a");
    EXPECT_EQ(folder.read("b.xml"), "new b");
    EXPECT_EQ(folder.names(), (std::vector<std::string>{"a.xml", "b.xml"}));
}

TEST(OutputFilesTest, AFailedRenameRestoresTheFilesAlreadyReplaced) {
    const TemporaryFolder folder;

    folder.write("a.xml", "old a");
    folder.write("a.xml.tmp", "new a");
    folder.write("b.xml.tmp", "new b");
    // A folder with a file in it where b.xml should go: a file cannot be renamed onto it.
    std::filesystem::create_directory(folder.path("b.xml"));
    folder.write("b.xml/keep.txt", "in the way");

    const auto result =
        replaceFiles({{.mTemporary = folder.path("a.xml.tmp"), .mFinal = folder.path("a.xml")},
                      {.mTemporary = folder.path("b.xml.tmp"), .mFinal = folder.path("b.xml")}});

    EXPECT_FALSE(result.mReplaced);
    EXPECT_TRUE(result.mNotRestored.empty());
    EXPECT_EQ(folder.read("a.xml"), "old a");
    EXPECT_EQ(folder.names(), (std::vector<std::string>{"a.xml", "b.xml"}));
    EXPECT_EQ(folder.read("b.xml/keep.txt"), "in the way");
}

TEST(OutputFilesTest, AFailedRenameRemovesANewFileThatHadNoOldOne) {
    const TemporaryFolder folder;

    folder.write("a.xml.tmp", "new a");
    folder.write("b.xml.tmp", "new b");
    std::filesystem::create_directory(folder.path("b.xml"));
    folder.write("b.xml/keep.txt", "in the way");

    const auto result =
        replaceFiles({{.mTemporary = folder.path("a.xml.tmp"), .mFinal = folder.path("a.xml")},
                      {.mTemporary = folder.path("b.xml.tmp"), .mFinal = folder.path("b.xml")}});

    EXPECT_FALSE(result.mReplaced);
    EXPECT_TRUE(result.mNotRestored.empty());
    EXPECT_EQ(folder.names(), (std::vector<std::string>{"b.xml"}));
}

TEST(OutputFilesTest, AnEarlierFileMovedAsideIsPutBackWhenItsNewFileIsMissing) {
    const TemporaryFolder folder;

    folder.write("a.xml", "old a");

    // There is no a.xml.tmp: the earlier file is moved to a.xml.old, the new file cannot take its
    // place, and the earlier file goes back.
    const auto result =
        replaceFiles({{.mTemporary = folder.path("a.xml.tmp"), .mFinal = folder.path("a.xml")}});

    EXPECT_FALSE(result.mReplaced);
    EXPECT_TRUE(result.mNotRestored.empty());
    EXPECT_EQ(folder.read("a.xml"), "old a");
    EXPECT_EQ(folder.names(), (std::vector<std::string>{"a.xml"}));
}

} // namespace
