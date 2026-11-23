#include "parsers/reference_catalogue.hpp"
#include "utils/country_codes.hpp"
#include "xml/xml_common.hpp"

#include <gtest/gtest.h>

namespace {
using namespace taxbroker;

TEST(CountryCodesTest, AnAssignedCountryIsAccepted) {
    EXPECT_TRUE(isIsoCountryCode("SI"));
    EXPECT_TRUE(isIsoCountryCode("US"));
    EXPECT_TRUE(isIsoCountryCode("JE")); // Jersey, an assigned code
    EXPECT_TRUE(isIsoCountryCode("KY")); // Cayman Islands
    EXPECT_TRUE(isIsoCountryCode("AD")); // first of the list
    EXPECT_TRUE(isIsoCountryCode("ZW")); // last of the list
}

TEST(CountryCodesTest, APlaceholderOrAnIsinPrefixIsNotACountry) {
    EXPECT_FALSE(isIsoCountryCode("XX")); // placeholder of the synthetic ISINs
    EXPECT_FALSE(isIsoCountryCode("XS")); // international securities
    EXPECT_FALSE(isIsoCountryCode("EU"));
    EXPECT_FALSE(isIsoCountryCode("XK")); // Kosovo has no ISO code
}

TEST(CountryCodesTest, TheTextMustBeExactlyTwoCapitalLetters) {
    EXPECT_FALSE(isIsoCountryCode("us")); // the list is in capital letters
    EXPECT_FALSE(isIsoCountryCode("Us"));
    EXPECT_FALSE(isIsoCountryCode("USA"));
    EXPECT_FALSE(isIsoCountryCode("U"));
    EXPECT_FALSE(isIsoCountryCode(""));
}

TEST(CountryCodesTest, TheXmlCheckUsesTheSameList) {
    EXPECT_TRUE(xml_common::isCountryCode("SI"));
    EXPECT_FALSE(xml_common::isCountryCode("XX"));
    EXPECT_FALSE(xml_common::isCountryCode("XS"));
    EXPECT_FALSE(xml_common::isCountryCode("EU"));
    EXPECT_FALSE(xml_common::isCountryCode("us"));
}

TEST(CountryCodesTest, EveryCountryOfTheBuiltInReferenceFilesIsInTheList) {
    const auto& catalogue = referenceCatalogue();

    for (const auto& payer : catalogue.payers())
    {
        EXPECT_TRUE(isIsoCountryCode(payer.mCountry)) << payer.mCountry;
    }

    for (const auto& statement : catalogue.allReliefStatements())
    {
        EXPECT_TRUE(isIsoCountryCode(statement.mCountry)) << statement.mCountry;
    }

    for (const auto& brokerPayer : catalogue.brokerPayers())
    {
        EXPECT_TRUE(isIsoCountryCode(brokerPayer.mCountry)) << brokerPayer.mCountry;
    }
}

} // namespace
