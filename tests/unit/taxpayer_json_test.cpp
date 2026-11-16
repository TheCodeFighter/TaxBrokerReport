#include "taxbroker/api/taxpayer_json.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <string>
#include <variant>

namespace {
using namespace taxbroker;
using taxbroker::api::parseTaxpayerJson;
using taxbroker::api::TaxpayerJsonError;

/// The error text, or a note that the file was accepted.
std::string errorOf(const std::variant<TaxpayerMetadata, TaxpayerJsonError>& aResult) {
    if (const auto* error = std::get_if<TaxpayerJsonError>(&aResult))
    {
        return error->mMessage;
    }

    return "accepted";
}

/// The key the error is about, or a note that the file was accepted.
std::string keyOf(const std::variant<TaxpayerMetadata, TaxpayerJsonError>& aResult) {
    if (const auto* error = std::get_if<TaxpayerJsonError>(&aResult))
    {
        return error->mKey;
    }

    return "accepted";
}

TEST(TaxpayerJsonTest, MinimalFileSetsOnlyTheTaxNumberAndTheResidence) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": true})");

    ASSERT_TRUE(std::holds_alternative<TaxpayerMetadata>(result));
    EXPECT_EQ(std::get<TaxpayerMetadata>(result),
              (TaxpayerMetadata{.mTaxNumber = "12345678", .mIsResident = true}));
}

TEST(TaxpayerJsonTest, EveryOptionalFieldIsMapped) {
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

TEST(TaxpayerJsonTest, MissingTaxNumberIsRefused) {
    const auto result = parseTaxpayerJson(R"({"isResident": true})");

    EXPECT_EQ(errorOf(result), "\"taxNumber\" is missing.");
}

TEST(TaxpayerJsonTest, TaxNumberWithSevenDigitsIsRefused) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "1234567", "isResident": true})");

    EXPECT_EQ(errorOf(result), "\"taxNumber\" must be a text of eight digits, not 00000000.");
}

TEST(TaxpayerJsonTest, TaxNumberOfZerosIsRefused) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "00000000", "isResident": true})");

    EXPECT_EQ(errorOf(result), "\"taxNumber\" must be a text of eight digits, not 00000000.");
}

TEST(TaxpayerJsonTest, TaxNumberWithALetterIsRefused) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "1234567A", "isResident": true})");

    EXPECT_EQ(errorOf(result), "\"taxNumber\" must be a text of eight digits, not 00000000.");
}

TEST(TaxpayerJsonTest, TaxNumberAsANumberIsRefused) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": 12345678, "isResident": true})");

    EXPECT_EQ(errorOf(result), "\"taxNumber\" must be a text of eight digits, not 00000000.");
}

TEST(TaxpayerJsonTest, MissingResidenceIsRefused) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "12345678"})");

    EXPECT_EQ(errorOf(result), "\"isResident\" is missing.");
}

TEST(TaxpayerJsonTest, ResidenceAsTextIsRefused) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": "yes"})");

    EXPECT_EQ(errorOf(result), "\"isResident\" must be true or false.");
}

TEST(TaxpayerJsonTest, NonResidentIsNotSupportedYet) {
    const auto result = parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": false})");

    EXPECT_EQ(errorOf(result),
              "\"isResident\" is false, but only Slovenian tax residents are supported for now.");
}

TEST(TaxpayerJsonTest, FilingTypeIsNotReadFromTheFile) {
    const auto result = parseTaxpayerJson(
        R"({"taxNumber": "12345678", "isResident": true, "filingType": "SelfReport"})");

    EXPECT_EQ(errorOf(result),
              "\"filingType\" is not read from the taxpayer file: the report is an original "
              "return, or a self-report with --self-report.");
}

TEST(TaxpayerJsonTest, UnknownKeyIsRefused) {
    // "postcode" instead of "postCode".
    const auto result =
        parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": true, "postcode": "1000"})");

    EXPECT_EQ(errorOf(result), "\"postcode\" is not a known key of the taxpayer file.");
}

TEST(TaxpayerJsonTest, EmptyNameIsRefused) {
    const auto result =
        parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": true, "name": ""})");

    EXPECT_EQ(errorOf(result), "\"name\" must be a text that is not empty.");
}

TEST(TaxpayerJsonTest, PostCodeAsANumberIsRefused) {
    const auto result =
        parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": true, "postCode": 1000})");

    EXPECT_EQ(errorOf(result), "\"postCode\" must be a text that is not empty.");
}

TEST(TaxpayerJsonTest, BirthDateThatDoesNotExistIsRefused) {
    // There is no 30 February.
    const auto result = parseTaxpayerJson(
        R"({"taxNumber": "12345678", "isResident": true, "birthDate": "1990-02-30"})");

    EXPECT_EQ(errorOf(result), "\"birthDate\" must be a date in the form YYYY-MM-DD.");
}

TEST(TaxpayerJsonTest, BirthDateAsANumberIsRefused) {
    const auto result = parseTaxpayerJson(
        R"({"taxNumber": "12345678", "isResident": true, "birthDate": 19900131})");

    EXPECT_EQ(errorOf(result), "\"birthDate\" must be a text that is not empty.");
}

TEST(TaxpayerJsonTest, TextThatIsNotJsonIsRefused) {
    // The closing brace is missing.
    const auto result = parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": true)");

    EXPECT_EQ(errorOf(result), "The taxpayer file is not valid JSON.");
}

TEST(TaxpayerJsonTest, JsonArrayIsRefused) {
    const auto result = parseTaxpayerJson(R"([{"taxNumber": "12345678", "isResident": true}])");

    EXPECT_EQ(errorOf(result), "The taxpayer file must contain one JSON object.");
}

TEST(TaxpayerJsonTest, ErrorsNameTheKeyTheyAreAbout) {
    EXPECT_EQ(keyOf(parseTaxpayerJson(R"({"taxNumber": "00000000", "isResident": true})")),
              "taxNumber");
    EXPECT_EQ(keyOf(parseTaxpayerJson(R"({"isResident": true})")), "taxNumber");
    EXPECT_EQ(keyOf(parseTaxpayerJson(R"({"taxNumber": "12345678"})")), "isResident");
    EXPECT_EQ(keyOf(parseTaxpayerJson(
                  R"({"taxNumber": "12345678", "isResident": true, "birthDate": "1990-02-30"})")),
              "birthDate");
    EXPECT_EQ(
        keyOf(parseTaxpayerJson(R"({"taxNumber": "12345678", "isResident": true, "name": ""})")),
        "name");
    EXPECT_EQ(keyOf(parseTaxpayerJson(
                  R"({"taxNumber": "12345678", "isResident": true, "filingType": "original"})")),
              "filingType");
}

TEST(TaxpayerJsonTest, ErrorsWithoutAKnownKeyHaveNoKey) {
    // Not JSON, not an object, and a key that the application does not know: the key is the
    // sender's text, so it is not repeated.
    EXPECT_EQ(keyOf(parseTaxpayerJson(R"({"taxNumber": "12345678")")), "");
    EXPECT_EQ(keyOf(parseTaxpayerJson("[]")), "");
    EXPECT_EQ(keyOf(parseTaxpayerJson(
                  R"({"taxNumber": "12345678", "isResident": true, "shoeSize": "42"})")),
              "");
}

} // namespace
