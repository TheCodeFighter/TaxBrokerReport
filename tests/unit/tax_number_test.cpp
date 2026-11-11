#include "taxbroker/tax_number.hpp"

#include <gtest/gtest.h>

namespace {
using namespace taxbroker;

TEST(TaxNumberTest, ANumberWithTheRightCheckDigitIsValid) {
    // 1234567: 8*1 + 7*2 + 6*3 + 5*4 + 4*5 + 3*6 + 2*7 = 112; 112 mod 11 = 2; 11 - 2 = 9.
    EXPECT_TRUE(isSlovenianTaxNumber("12345679"));
}

TEST(TaxNumberTest, ANumberWithAWrongCheckDigitIsNotValid) {
    // Same first seven digits as above: the check digit must be 9.
    EXPECT_FALSE(isSlovenianTaxNumber("12345674"));
}

TEST(TaxNumberTest, ACheckDigitOfTenIsWrittenAsZero) {
    // 1000002: 8*1 + 2*2 = 12; 12 mod 11 = 1; 11 - 1 = 10, written as 0.
    EXPECT_TRUE(isSlovenianTaxNumber("10000020"));
    EXPECT_FALSE(isSlovenianTaxNumber("10000029"));
}

TEST(TaxNumberTest, ASumThatIsAMultipleOfElevenHasNoCheckDigit) {
    // 1000010: 8*1 + 3*1 = 11; 11 mod 11 = 0, so no digit fits: neither 0 nor 1 is valid.
    EXPECT_FALSE(isSlovenianTaxNumber("10000100"));
    EXPECT_FALSE(isSlovenianTaxNumber("10000101"));
}

TEST(TaxNumberTest, EightZerosAreNotValid) {
    EXPECT_FALSE(isSlovenianTaxNumber("00000000"));
}

TEST(TaxNumberTest, TheNumbersOfSlovenianCompaniesInTheTaxRegisterAreValid) {
    EXPECT_TRUE(isSlovenianTaxNumber("82646716"));
    EXPECT_TRUE(isSlovenianTaxNumber("91132550"));
    EXPECT_TRUE(isSlovenianTaxNumber("80267432"));
    EXPECT_TRUE(isSlovenianTaxNumber("89190033"));
}

TEST(TaxNumberTest, TheOldKrkaNumberOfTheReferenceFileIsNotValid) {
    // Sum 116, 116 mod 11 = 6, check digit 5, but the number ends in 0.
    EXPECT_FALSE(isSlovenianTaxNumber("61701500"));
}

TEST(TaxNumberTest, ThePrefixSiAndWrongLengthsAreNotValid) {
    EXPECT_FALSE(isSlovenianTaxNumber("SI12345679"));
    EXPECT_FALSE(isSlovenianTaxNumber("1234567"));
    EXPECT_FALSE(isSlovenianTaxNumber("123456790"));
    EXPECT_FALSE(isSlovenianTaxNumber(""));
}

TEST(TaxNumberTest, ANumberWithANonDigitIsNotValid) {
    EXPECT_FALSE(isSlovenianTaxNumber("1234567A"));
    EXPECT_FALSE(isSlovenianTaxNumber("12 45679"));
}

} // namespace
