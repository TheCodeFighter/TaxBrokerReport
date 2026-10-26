#include "cli/taxpayer_file.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

namespace {
using namespace taxbroker;
using taxbroker::cli::parseTaxpayerJson;
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

TEST(TaxpayerFileTest, MinimalFileSetsOnlyTheTaxNumberAndTheResidence) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": true})");

    ASSERT_TRUE(std::holds_alternative<TaxpayerMetadata>(result));
    EXPECT_EQ(std::get<TaxpayerMetadata>(result),
              (TaxpayerMetadata{.mTaxNumber = "12345678", .mIsResident = true}));
}

TEST(TaxpayerFileTest, EveryOptionalFieldIsMapped) {
    const auto result = parseTaxpayerJson(R"({
        "taxNumber": "12345678",
        "isResident": true,
        "name": "Ana Novak",
        "address": "Slovenska cesta 1",
        "city": "Ljubljana",
        "postCode": "1000",
        "postName": "Ljubljana",
        "birthDate": "1990-01-31",
        "email": "ana.novak@example.com",
        "phoneNumber": "040123456"
    })");

    ASSERT_TRUE(std::holds_alternative<TaxpayerMetadata>(result));
    EXPECT_EQ(std::get<TaxpayerMetadata>(result),
              (TaxpayerMetadata{
                  .mTaxNumber = "12345678",
                  .mName = "Ana Novak",
                  .mAddress = "Slovenska cesta 1",
                  .mCity = "Ljubljana",
                  .mPostCode = "1000",
                  .mPostName = "Ljubljana",
                  .mBirthDate = parseCalendarDate("1990-01-31"),
                  .mIsResident = true,
                  .mEmail = "ana.novak@example.com",
                  .mPhoneNumber = "040123456",
              }));
}

TEST(TaxpayerFileTest, MissingTaxNumberIsRefused) {
    const auto result = parseTaxpayerJson(R"({"isResident": true})");

    EXPECT_EQ(errorOf(result), "\"taxNumber\" is missing.");
}

TEST(TaxpayerFileTest, TaxNumberWithSevenDigitsIsRefused) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "1234567", "isResident": true})");

    EXPECT_EQ(errorOf(result), "\"taxNumber\" must be a text of eight digits, not 00000000.");
}

TEST(TaxpayerFileTest, TaxNumberOfZerosIsRefused) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "00000000", "isResident": true})");

    EXPECT_EQ(errorOf(result), "\"taxNumber\" must be a text of eight digits, not 00000000.");
}

TEST(TaxpayerFileTest, TaxNumberWithALetterIsRefused) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "1234567A", "isResident": true})");

    EXPECT_EQ(errorOf(result), "\"taxNumber\" must be a text of eight digits, not 00000000.");
}

TEST(TaxpayerFileTest, TaxNumberAsANumberIsRefused) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": 12345678, "isResident": true})");

    EXPECT_EQ(errorOf(result), "\"taxNumber\" must be a text of eight digits, not 00000000.");
}

TEST(TaxpayerFileTest, MissingResidenceIsRefused) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "12345678"})");

    EXPECT_EQ(errorOf(result), "\"isResident\" is missing.");
}

TEST(TaxpayerFileTest, ResidenceAsTextIsRefused) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": "yes"})");

    EXPECT_EQ(errorOf(result), "\"isResident\" must be true or false.");
}

TEST(TaxpayerFileTest, NonResidentIsNotSupportedYet) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": false})");

    EXPECT_EQ(errorOf(result),
              "\"isResident\" is false, but only Slovenian tax residents are supported for now.");
}

TEST(TaxpayerFileTest, FilingTypeIsNotReadFromTheFile) {
    const auto result = parseTaxpayerJson(
        R"({"taxNumber": "12345678", "isResident": true, "filingType": "SelfReport"})");

    EXPECT_EQ(errorOf(result),
              "\"filingType\" is not read from the taxpayer file: the report is an original "
              "return, or a self-report with --self-report.");
}

TEST(TaxpayerFileTest, UnknownKeyIsRefused) {
    // "postcode" instead of "postCode".
    const auto result =
        parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": true, "postcode": "1000"})");

    EXPECT_EQ(errorOf(result), "\"postcode\" is not a known key of the taxpayer file.");
}

TEST(TaxpayerFileTest, EmptyNameIsRefused) {
    const auto result =
        parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": true, "name": ""})");

    EXPECT_EQ(errorOf(result), "\"name\" must be a text that is not empty.");
}

TEST(TaxpayerFileTest, PostCodeAsANumberIsRefused) {
    const auto result =
        parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": true, "postCode": 1000})");

    EXPECT_EQ(errorOf(result), "\"postCode\" must be a text that is not empty.");
}

TEST(TaxpayerFileTest, BirthDateThatDoesNotExistIsRefused) {
    // There is no 30 February.
    const auto result = parseTaxpayerJson(
        R"({"taxNumber": "12345678", "isResident": true, "birthDate": "1990-02-30"})");

    EXPECT_EQ(errorOf(result), "\"birthDate\" must be a date in the form YYYY-MM-DD.");
}

TEST(TaxpayerFileTest, BirthDateAsANumberIsRefused) {
    const auto result = parseTaxpayerJson(
        R"({"taxNumber": "12345678", "isResident": true, "birthDate": 19900131})");

    EXPECT_EQ(errorOf(result), "\"birthDate\" must be a text that is not empty.");
}

TEST(TaxpayerFileTest, TextThatIsNotJsonIsRefused) {
    // The closing brace is missing.
    const auto result = parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": true)");

    EXPECT_EQ(errorOf(result), "The taxpayer file is not valid JSON.");
}

TEST(TaxpayerFileTest, JsonArrayIsRefused) {
    const auto result = parseTaxpayerJson(R"([{"taxNumber": "12345678", "isResident": true}])");

    EXPECT_EQ(errorOf(result), "The taxpayer file must contain one JSON object.");
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

TEST(TaxpayerFileTest, MissingFileCannotBeRead) {
    const auto result = readTaxpayerFile(std::filesystem::temp_directory_path() /
                                         "taxbroker_taxpayer_that_does_not_exist.json");

    EXPECT_EQ(errorOf(result), "The taxpayer file cannot be read.");
}

} // namespace
