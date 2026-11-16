#include "taxbroker/api/taxpayer_json.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace {
using taxbroker::api::readTaxpayerProfile;
using taxbroker::api::TaxpayerProfile;

using Fields = std::map<std::string, std::string>;
using Keys = std::vector<std::string>;

TEST(ProfileJsonTest, AllValidKeysAreRead) {
    const TaxpayerProfile profile =
        readTaxpayerProfile(R"({"taxNumber":"12345679","isResident":true,"name":"Ana Novak"})");

    EXPECT_EQ(profile.mFields, (Fields{{"name", "Ana Novak"}, {"taxNumber", "12345679"}}));
    EXPECT_TRUE(profile.mIsResident);
    EXPECT_EQ(profile.mInvalidKeys, Keys{});
    EXPECT_FALSE(profile.mUnreadable);
    EXPECT_FALSE(profile.mIgnoredKeys);
}

TEST(ProfileJsonTest, EveryTextKeyIsRead) {
    const TaxpayerProfile profile = readTaxpayerProfile(R"({
        "taxNumber": "12345679",
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

    EXPECT_EQ(profile.mFields,
              (Fields{{"address", "Slovenska cesta 1"},
                      {"birthDate", "1990-01-31"},
                      {"city", "Ljubljana"},
                      {"email", "ana.novak@example.com"},
                      {"name", "Ana Novak"},
                      {"phoneNumber", "040123456"},
                      {"postCode", "1000"},
                      {"postName", "Ljubljana"},
                      {"taxNumber", "12345679"}}));
    EXPECT_EQ(profile.mInvalidKeys, Keys{});
}

TEST(ProfileJsonTest, SlovenianLettersInEveryTextKeyAreReadUnchanged) {
    const TaxpayerProfile profile = readTaxpayerProfile(R"({
        "taxNumber": "12345679",
        "isResident": true,
        "name": "Žiga Šuštaršič-Čeh",
        "address": "Črnuška cesta 12, Šmartno ob Paki",
        "city": "Črnomelj",
        "postName": "Škofja Loka"
    })");

    EXPECT_EQ(profile.mFields,
              (Fields{{"address", "Črnuška cesta 12, Šmartno ob Paki"},
                      {"city", "Črnomelj"},
                      {"name", "Žiga Šuštaršič-Čeh"},
                      {"postName", "Škofja Loka"},
                      {"taxNumber", "12345679"}}));
    EXPECT_EQ(profile.mInvalidKeys, Keys{});
}

TEST(ProfileJsonTest, ANameThatFillsTheFileTo65536BytesIsReadAndOneByteMoreIsUnreadable) {
    const std::string head = R"({"taxNumber":"12345679","isResident":true,"name":")";
    const std::string tail = R"("})";
    // Each "Č" is two bytes; plain "a" letters fill the rest.
    const std::size_t filler = 65536 - head.size() - tail.size();
    std::string name;

    for (std::size_t index = 0; index < filler / 2; ++index)
    {
        name += "Č";
    }

    name += std::string(filler % 2, 'a');

    const std::string fits = head + name + tail;
    const std::string tooLarge = head + name + "a" + tail;

    ASSERT_EQ(fits.size(), 65536U);
    ASSERT_EQ(tooLarge.size(), 65537U);

    const TaxpayerProfile read = readTaxpayerProfile(fits);

    EXPECT_FALSE(read.mUnreadable);
    EXPECT_EQ(read.mFields.at("name"), name);
    EXPECT_TRUE(readTaxpayerProfile(tooLarge).mUnreadable);
}

TEST(ProfileJsonTest, ATaxNumberOfSevenDigitsIsInvalidAndTheRestIsKept) {
    const TaxpayerProfile profile =
        readTaxpayerProfile(R"({"taxNumber":"1234567","isResident":true,"name":"Ana Novak"})");

    // Seven digits instead of eight.
    EXPECT_EQ(profile.mFields, (Fields{{"name", "Ana Novak"}}));
    EXPECT_EQ(profile.mInvalidKeys, (Keys{"taxNumber"}));
    EXPECT_FALSE(profile.mUnreadable);
    EXPECT_FALSE(profile.mIgnoredKeys);
}

TEST(ProfileJsonTest, ATaxNumberWithAWrongCheckDigitIsKeptAsTyped) {
    const TaxpayerProfile profile =
        readTaxpayerProfile(R"({"taxNumber":"12345678","isResident":true,"name":"Ana Novak"})");

    // 1*8 + 2*7 + 3*6 + 4*5 + 5*4 + 6*3 + 7*2 = 112; 112 mod 11 = 2; the check digit is 9, not 8.
    // The run warns about it; the saved profile does not refuse it.
    EXPECT_EQ(profile.mFields, (Fields{{"name", "Ana Novak"}, {"taxNumber", "12345678"}}));
    EXPECT_EQ(profile.mInvalidKeys, Keys{});
    EXPECT_FALSE(profile.mUnreadable);
}

TEST(ProfileJsonTest, APostCodeOutsideFourDigitsFrom1000IsInvalidAndTheRestIsKept) {
    const TaxpayerProfile profile = readTaxpayerProfile(
        R"({"taxNumber":"12345679","isResident":true,"postCode":"999","postName":"Škofja Loka"})");

    EXPECT_EQ(profile.mFields, (Fields{{"postName", "Škofja Loka"}, {"taxNumber", "12345679"}}));
    EXPECT_EQ(profile.mInvalidKeys, (Keys{"postCode"}));
}

TEST(ProfileJsonTest, ABirthDateThatDoesNotExistIsInvalid) {
    const TaxpayerProfile profile = readTaxpayerProfile(
        R"({"taxNumber":"12345679","isResident":true,"birthDate":"1980-02-30"})");

    // There is no 30 February.
    EXPECT_EQ(profile.mFields, (Fields{{"taxNumber", "12345679"}}));
    EXPECT_EQ(profile.mInvalidKeys, (Keys{"birthDate"}));
}

TEST(ProfileJsonTest, AnEmptyTextIsInvalid) {
    const TaxpayerProfile profile =
        readTaxpayerProfile(R"({"taxNumber":"12345679","isResident":true,"city":""})");

    EXPECT_EQ(profile.mFields, (Fields{{"taxNumber", "12345679"}}));
    EXPECT_EQ(profile.mInvalidKeys, (Keys{"city"}));
}

TEST(ProfileJsonTest, ANumberInsteadOfATextIsInvalid) {
    const TaxpayerProfile profile =
        readTaxpayerProfile(R"({"taxNumber":"12345679","isResident":true,"email":5})");

    EXPECT_EQ(profile.mFields, (Fields{{"taxNumber", "12345679"}}));
    EXPECT_EQ(profile.mInvalidKeys, (Keys{"email"}));
}

TEST(ProfileJsonTest, SeveralInvalidKeysAreListedInKeyOrder) {
    const TaxpayerProfile profile =
        readTaxpayerProfile(R"({"taxNumber":"0","isResident":true,"name":"","birthDate":"x"})");

    EXPECT_EQ(profile.mFields, (Fields{}));
    EXPECT_EQ(profile.mInvalidKeys, (Keys{"birthDate", "name", "taxNumber"}));
}

TEST(ProfileJsonTest, AnUnknownKeyIsIgnoredWithoutBeingNamed) {
    const TaxpayerProfile profile = readTaxpayerProfile(
        R"({"taxNumber":"12345679","isResident":true,"filingType":"original"})");

    EXPECT_EQ(profile.mFields, (Fields{{"taxNumber", "12345679"}}));
    EXPECT_EQ(profile.mInvalidKeys, Keys{});
    EXPECT_FALSE(profile.mUnreadable);
    EXPECT_TRUE(profile.mIgnoredKeys);
}

TEST(ProfileJsonTest, ResidentFalseIsInvalid) {
    const TaxpayerProfile profile =
        readTaxpayerProfile(R"({"taxNumber":"12345679","isResident":false})");

    EXPECT_EQ(profile.mFields, (Fields{{"taxNumber", "12345679"}}));
    EXPECT_FALSE(profile.mIsResident);
    EXPECT_EQ(profile.mInvalidKeys, (Keys{"isResident"}));
}

TEST(ProfileJsonTest, MissingKeysAreEmptyAndNotInvalid) {
    const TaxpayerProfile profile = readTaxpayerProfile(R"({"name":"Ana Novak"})");

    EXPECT_EQ(profile.mFields, (Fields{{"name", "Ana Novak"}}));
    EXPECT_FALSE(profile.mIsResident);
    EXPECT_EQ(profile.mInvalidKeys, Keys{});
    EXPECT_FALSE(profile.mUnreadable);
    EXPECT_FALSE(profile.mIgnoredKeys);
}

TEST(ProfileJsonTest, TextThatIsNotJsonIsUnreadable) {
    const TaxpayerProfile profile = readTaxpayerProfile("not json");

    EXPECT_TRUE(profile.mUnreadable);
    EXPECT_EQ(profile.mFields, (Fields{}));
    EXPECT_EQ(profile.mInvalidKeys, Keys{});
    EXPECT_FALSE(profile.mIgnoredKeys);
}

TEST(ProfileJsonTest, AJsonArrayIsUnreadable) {
    const TaxpayerProfile profile = readTaxpayerProfile("[]");

    EXPECT_TRUE(profile.mUnreadable);
    EXPECT_EQ(profile.mFields, (Fields{}));
}

TEST(ProfileJsonTest, AFileOf65537BytesIsUnreadable) {
    const std::string object = R"({"taxNumber":"12345679","isResident":true})";
    const std::string text = object + std::string(65537 - object.size(), ' ');

    ASSERT_EQ(text.size(), 65537U);

    const TaxpayerProfile profile = readTaxpayerProfile(text);

    EXPECT_TRUE(profile.mUnreadable);
    EXPECT_EQ(profile.mFields, (Fields{}));
}

TEST(ProfileJsonTest, AFileOf65536BytesIsRead) {
    const std::string object = R"({"taxNumber":"12345679","isResident":true})";
    const std::string text = object + std::string(65536 - object.size(), ' ');

    ASSERT_EQ(text.size(), 65536U);

    const TaxpayerProfile profile = readTaxpayerProfile(text);

    EXPECT_FALSE(profile.mUnreadable);
    EXPECT_EQ(profile.mFields, (Fields{{"taxNumber", "12345679"}}));
}

} // namespace
