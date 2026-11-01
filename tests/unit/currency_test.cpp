#include "taxbroker/currency.hpp"

#include <gtest/gtest.h>

namespace {
using namespace taxbroker;

TEST(CurrencyTest, ParsesTheCodesOfTheNewlySupportedCurrencies) {
    EXPECT_EQ(parseCurrencyCode("DKK"), Currency::DKK);
    EXPECT_EQ(parseCurrencyCode("SEK"), Currency::SEK);
    EXPECT_EQ(parseCurrencyCode("NOK"), Currency::NOK);
    EXPECT_EQ(parseCurrencyCode("CAD"), Currency::CAD);
    EXPECT_EQ(parseCurrencyCode("AUD"), Currency::AUD);
    EXPECT_EQ(parseCurrencyCode("HKD"), Currency::HKD);
}

TEST(CurrencyTest, KeepsTheFirstFiveCurrencies) {
    EXPECT_EQ(parseCurrencyCode("EUR"), Currency::EUR);
    EXPECT_EQ(parseCurrencyCode("USD"), Currency::USD);
    EXPECT_EQ(parseCurrencyCode("GBP"), Currency::GBP);
    EXPECT_EQ(parseCurrencyCode("CHF"), Currency::CHF);
    EXPECT_EQ(parseCurrencyCode("JPY"), Currency::JPY);
}

TEST(CurrencyTest, ParsesCodesThatTheEcbStoppedPublishingAfter2020) {
    // HRK ended on 2022-12-30 and RUB on 2022-03-01; both still have ECB rates in 2020 and later.
    EXPECT_EQ(parseCurrencyCode("HRK"), Currency::HRK);
    EXPECT_EQ(parseCurrencyCode("RUB"), Currency::RUB);
}

TEST(CurrencyTest, RejectsCodesWithoutAnEcbRateSince2020) {
    // The Slovenian tolar, ended 2006.
    EXPECT_EQ(parseCurrencyCode("SIT"), Currency::Unknown);
    // The Slovak koruna, ended 2008.
    EXPECT_EQ(parseCurrencyCode("SKK"), Currency::Unknown);
    EXPECT_EQ(parseCurrencyCode("XXX"), Currency::Unknown);
}

TEST(CurrencyTest, RejectsTextThatIsNotAnExactUppercaseCode) {
    EXPECT_EQ(parseCurrencyCode("nok"), Currency::Unknown);
    EXPECT_EQ(parseCurrencyCode("Nok"), Currency::Unknown);
    EXPECT_EQ(parseCurrencyCode(""), Currency::Unknown);
    EXPECT_EQ(parseCurrencyCode("NO"), Currency::Unknown);
    EXPECT_EQ(parseCurrencyCode("NOKX"), Currency::Unknown);
    EXPECT_EQ(parseCurrencyCode(" NOK"), Currency::Unknown);
}

TEST(CurrencyTest, ParsesEveryCodeOfTheSupportedSet) {
    // EUR and the 32 codes the ECB published on or after 2020-01-01.
    EXPECT_EQ(parseCurrencyCode("EUR"), Currency::EUR);
    EXPECT_EQ(parseCurrencyCode("USD"), Currency::USD);
    EXPECT_EQ(parseCurrencyCode("GBP"), Currency::GBP);
    EXPECT_EQ(parseCurrencyCode("CHF"), Currency::CHF);
    EXPECT_EQ(parseCurrencyCode("JPY"), Currency::JPY);
    EXPECT_EQ(parseCurrencyCode("AUD"), Currency::AUD);
    EXPECT_EQ(parseCurrencyCode("BGN"), Currency::BGN);
    EXPECT_EQ(parseCurrencyCode("BRL"), Currency::BRL);
    EXPECT_EQ(parseCurrencyCode("CAD"), Currency::CAD);
    EXPECT_EQ(parseCurrencyCode("CNY"), Currency::CNY);
    EXPECT_EQ(parseCurrencyCode("CZK"), Currency::CZK);
    EXPECT_EQ(parseCurrencyCode("DKK"), Currency::DKK);
    EXPECT_EQ(parseCurrencyCode("HKD"), Currency::HKD);
    EXPECT_EQ(parseCurrencyCode("HRK"), Currency::HRK);
    EXPECT_EQ(parseCurrencyCode("HUF"), Currency::HUF);
    EXPECT_EQ(parseCurrencyCode("IDR"), Currency::IDR);
    EXPECT_EQ(parseCurrencyCode("ILS"), Currency::ILS);
    EXPECT_EQ(parseCurrencyCode("INR"), Currency::INR);
    EXPECT_EQ(parseCurrencyCode("ISK"), Currency::ISK);
    EXPECT_EQ(parseCurrencyCode("KRW"), Currency::KRW);
    EXPECT_EQ(parseCurrencyCode("MXN"), Currency::MXN);
    EXPECT_EQ(parseCurrencyCode("MYR"), Currency::MYR);
    EXPECT_EQ(parseCurrencyCode("NOK"), Currency::NOK);
    EXPECT_EQ(parseCurrencyCode("NZD"), Currency::NZD);
    EXPECT_EQ(parseCurrencyCode("PHP"), Currency::PHP);
    EXPECT_EQ(parseCurrencyCode("PLN"), Currency::PLN);
    EXPECT_EQ(parseCurrencyCode("RON"), Currency::RON);
    EXPECT_EQ(parseCurrencyCode("RUB"), Currency::RUB);
    EXPECT_EQ(parseCurrencyCode("SEK"), Currency::SEK);
    EXPECT_EQ(parseCurrencyCode("SGD"), Currency::SGD);
    EXPECT_EQ(parseCurrencyCode("THB"), Currency::THB);
    EXPECT_EQ(parseCurrencyCode("TRY"), Currency::TRY);
    EXPECT_EQ(parseCurrencyCode("ZAR"), Currency::ZAR);
}

TEST(CurrencyTest, WritesEveryCodeOfTheSupportedSet) {
    EXPECT_EQ(currencyCode(Currency::EUR), "EUR");
    EXPECT_EQ(currencyCode(Currency::USD), "USD");
    EXPECT_EQ(currencyCode(Currency::GBP), "GBP");
    EXPECT_EQ(currencyCode(Currency::CHF), "CHF");
    EXPECT_EQ(currencyCode(Currency::JPY), "JPY");
    EXPECT_EQ(currencyCode(Currency::AUD), "AUD");
    EXPECT_EQ(currencyCode(Currency::BGN), "BGN");
    EXPECT_EQ(currencyCode(Currency::BRL), "BRL");
    EXPECT_EQ(currencyCode(Currency::CAD), "CAD");
    EXPECT_EQ(currencyCode(Currency::CNY), "CNY");
    EXPECT_EQ(currencyCode(Currency::CZK), "CZK");
    EXPECT_EQ(currencyCode(Currency::DKK), "DKK");
    EXPECT_EQ(currencyCode(Currency::HKD), "HKD");
    EXPECT_EQ(currencyCode(Currency::HRK), "HRK");
    EXPECT_EQ(currencyCode(Currency::HUF), "HUF");
    EXPECT_EQ(currencyCode(Currency::IDR), "IDR");
    EXPECT_EQ(currencyCode(Currency::ILS), "ILS");
    EXPECT_EQ(currencyCode(Currency::INR), "INR");
    EXPECT_EQ(currencyCode(Currency::ISK), "ISK");
    EXPECT_EQ(currencyCode(Currency::KRW), "KRW");
    EXPECT_EQ(currencyCode(Currency::MXN), "MXN");
    EXPECT_EQ(currencyCode(Currency::MYR), "MYR");
    EXPECT_EQ(currencyCode(Currency::NOK), "NOK");
    EXPECT_EQ(currencyCode(Currency::NZD), "NZD");
    EXPECT_EQ(currencyCode(Currency::PHP), "PHP");
    EXPECT_EQ(currencyCode(Currency::PLN), "PLN");
    EXPECT_EQ(currencyCode(Currency::RON), "RON");
    EXPECT_EQ(currencyCode(Currency::RUB), "RUB");
    EXPECT_EQ(currencyCode(Currency::SEK), "SEK");
    EXPECT_EQ(currencyCode(Currency::SGD), "SGD");
    EXPECT_EQ(currencyCode(Currency::THB), "THB");
    EXPECT_EQ(currencyCode(Currency::TRY), "TRY");
    EXPECT_EQ(currencyCode(Currency::ZAR), "ZAR");
}

TEST(CurrencyTest, GivesAnEmptyCodeForAnUnknownCurrency) {
    EXPECT_EQ(currencyCode(Currency::Unknown), "");
}

} // namespace
