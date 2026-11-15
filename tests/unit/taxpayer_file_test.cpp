#include "cli/taxpayer_file.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

namespace {
using namespace taxbroker;
using taxbroker::cli::readTaxpayerFile;
using taxbroker::cli::TaxpayerFileError;

/// The error text, or a note that the file was accepted.
std::string errorOf(const std::variant<TaxpayerMetadata, TaxpayerFileError>& aResult) {
    if (const auto* error = std::get_if<TaxpayerFileError>(&aResult))
    {
        return error->mMessage;
    }

    return "accepted";
}

TEST(TaxpayerFileTest, ReadsTheFileFromDisk) {
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path =
        std::filesystem::temp_directory_path() / ("taxbroker_taxpayer_" + std::to_string(token));

    {
        std::ofstream output{path};
        output << R"({"taxNumber": "12345678", "isResident": true})";
    }

    const auto result = readTaxpayerFile(path);
    std::filesystem::remove(path);

    ASSERT_TRUE(std::holds_alternative<TaxpayerMetadata>(result));
    EXPECT_EQ(std::get<TaxpayerMetadata>(result).mTaxNumber, "12345678");
}

TEST(TaxpayerFileTest, TheFileAndItsTextGiveTheSameTaxpayer) {
    const std::string text =
        R"({"taxNumber": "12345678", "isResident": true, "name": "Ana Novak", "city": "Ljubljana"})";
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("taxbroker_taxpayer_same_" + std::to_string(token));

    {
        std::ofstream output{path};
        output << text;
    }

    const auto fromFile = readTaxpayerFile(path);

    std::filesystem::remove(path);

    const auto fromText = api::parseTaxpayerJson(text);

    ASSERT_TRUE(std::holds_alternative<TaxpayerMetadata>(fromFile));
    ASSERT_TRUE(std::holds_alternative<TaxpayerMetadata>(fromText));
    EXPECT_EQ(std::get<TaxpayerMetadata>(fromFile), std::get<TaxpayerMetadata>(fromText));
    EXPECT_EQ(std::get<TaxpayerMetadata>(fromText).mName, "Ana Novak");
}

TEST(TaxpayerFileTest, FolderCannotBeRead) {
    const auto folder = std::filesystem::temp_directory_path() / "taxbroker_taxpayer_folder.json";

    std::filesystem::create_directories(folder);

    const auto result = readTaxpayerFile(folder);

    std::filesystem::remove(folder);

    EXPECT_EQ(errorOf(result), "The taxpayer file cannot be read.");
}

TEST(TaxpayerFileTest, MissingFileCannotBeRead) {
    const auto result = readTaxpayerFile(std::filesystem::temp_directory_path() /
                                         "taxbroker_taxpayer_that_does_not_exist.json");

    EXPECT_EQ(errorOf(result), "The taxpayer file cannot be read.");
}

} // namespace
