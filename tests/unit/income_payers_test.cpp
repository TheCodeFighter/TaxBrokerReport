#include "processors/income_payers.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace {
using namespace taxbroker;

TEST(IncomePayersTest, ASlovenianPayerNumberWithThePrefixBecomesTheTaxNumber) {
    // `SI12345674` is a Slovenian VAT number: the forms want the eight digits as the tax number.
    const PayerEntry entry{
        .mIsin = "SI0000000001",
        .mIsinSource = "SI0000000001",
        .mName = "Example Izdajatelj d.d.",
        .mTaxNumber = "SI12345674",
        .mAddress = "1 Main St, Ljubljana",
        .mCountry = "SI",
        .mInstrumentKind = std::nullopt,
        .mVerifiedOn = std::nullopt,
        .mSource = {.mSource = "manual", .mFile = "payers.csv", .mRowNumber = 2},
    };

    const auto outcome = payerOutcomeOf(entry);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::optional<std::string>{"12345674"});
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mCountry, std::optional<std::string>{"SI"});
    EXPECT_TRUE(outcome.mIsSlovenian);
}

TEST(IncomePayersTest, AForeignPayerNumberStaysTheIdentificationNumber) {
    // A German number that starts with letters is not Slovenian, and is not changed.
    const PayerEntry entry{
        .mIsin = "DE0000000001",
        .mIsinSource = "DE0000000001",
        .mName = "Example AG",
        .mTaxNumber = "DE123456789",
        .mAddress = "1 Main St, Berlin",
        .mCountry = "DE",
        .mInstrumentKind = std::nullopt,
        .mVerifiedOn = std::nullopt,
        .mSource = {.mSource = "manual", .mFile = "payers.csv", .mRowNumber = 2},
    };

    const auto outcome = payerOutcomeOf(entry);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::optional<std::string>{"DE123456789"});
    EXPECT_FALSE(outcome.mIsSlovenian);
}

TEST(IncomePayersTest, ASlovenianPayerNumberWithoutThePrefixStaysTheIdentificationNumber) {
    // Eight digits without `SI` are not recognised as a VAT number, so nothing is guessed.
    const PayerEntry entry{
        .mIsin = "SI0000000001",
        .mIsinSource = "SI0000000001",
        .mName = "Example Izdajatelj d.d.",
        .mTaxNumber = "12345674",
        .mAddress = "1 Main St, Ljubljana",
        .mCountry = "SI",
        .mInstrumentKind = std::nullopt,
        .mVerifiedOn = std::nullopt,
        .mSource = {.mSource = "manual", .mFile = "payers.csv", .mRowNumber = 2},
    };

    const auto outcome = payerOutcomeOf(entry);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::optional<std::string>{"12345674"});
}

TEST(IncomePayersTest, ASlovenianPayerNumberOfEightZerosStaysTheIdentificationNumber) {
    // A tax number of eight zeros is not a positive integer, so it is not moved.
    const PayerEntry entry{
        .mIsin = "SI0000000001",
        .mIsinSource = "SI0000000001",
        .mName = "Example Izdajatelj d.d.",
        .mTaxNumber = "SI00000000",
        .mAddress = "1 Main St, Ljubljana",
        .mCountry = "SI",
        .mInstrumentKind = std::nullopt,
        .mVerifiedOn = std::nullopt,
        .mSource = {.mSource = "manual", .mFile = "payers.csv", .mRowNumber = 2},
    };

    const auto outcome = payerOutcomeOf(entry);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::optional<std::string>{"SI00000000"});
}

TEST(IncomePayersTest, ASlovenianPayerWithoutANumberHasNeitherNumber) {
    const PayerEntry entry{
        .mIsin = "SI0000000001",
        .mIsinSource = "SI0000000001",
        .mName = "Example Izdajatelj d.d.",
        .mTaxNumber = std::nullopt,
        .mAddress = "1 Main St, Ljubljana",
        .mCountry = "SI",
        .mInstrumentKind = std::nullopt,
        .mVerifiedOn = std::nullopt,
        .mSource = {.mSource = "manual", .mFile = "payers.csv", .mRowNumber = 2},
    };

    const auto outcome = payerOutcomeOf(entry);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::nullopt);
}

} // namespace
