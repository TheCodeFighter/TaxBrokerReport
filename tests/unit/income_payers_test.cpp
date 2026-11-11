#include "processors/income_payers.hpp"

#include "taxbroker/reference_catalogue.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace {
using namespace taxbroker;

TEST(IncomePayersTest, ASlovenianPayerNumberOfEightDigitsBecomesTheTaxNumber) {
    // 12345679 passes the check digit (see the tax number tests): the form wants it as the tax
    // number.
    const PayerEntry entry{
        .mIsin = "SI0000000001",
        .mIsinSource = "SI0000000001",
        .mName = "Example Izdajatelj d.d.",
        .mTaxNumber = "12345679",
        .mAddress = "1 Main St, Ljubljana",
        .mCountry = "SI",
        .mInstrumentKind = std::nullopt,
        .mVerifiedOn = std::nullopt,
        .mSource = {.mSource = "manual", .mFile = "payers.csv", .mRowNumber = 2},
    };

    const auto outcome = payerOutcomeOf(entry);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::optional<std::string>{"12345679"});
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mCountry, std::optional<std::string>{"SI"});
    EXPECT_TRUE(outcome.mIsSlovenian);
    EXPECT_FALSE(outcome.mSlovenianTaxNumberMissing);
}

TEST(IncomePayersTest, ASlovenianPayerNumberWithThePrefixIsNotATaxNumber) {
    // The number is the eight digits only; `SI` and the digits is not a tax number.
    const PayerEntry entry{
        .mIsin = "SI0000000001",
        .mIsinSource = "SI0000000001",
        .mName = "Example Izdajatelj d.d.",
        .mTaxNumber = "SI12345679",
        .mAddress = "1 Main St, Ljubljana",
        .mCountry = "SI",
        .mInstrumentKind = std::nullopt,
        .mVerifiedOn = std::nullopt,
        .mSource = {.mSource = "manual", .mFile = "payers.csv", .mRowNumber = 2},
    };

    const auto outcome = payerOutcomeOf(entry);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mName, std::optional<std::string>{"Example Izdajatelj d.d."});
    EXPECT_TRUE(outcome.mIsSlovenian);
    EXPECT_TRUE(outcome.mSlovenianTaxNumberMissing);
}

TEST(IncomePayersTest, ASlovenianPayerNumberWithAWrongCheckDigitIsNotWritten) {
    // 12345674 ends in 4, but the check digit of 1234567 is 9.
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
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mName, std::optional<std::string>{"Example Izdajatelj d.d."});
    EXPECT_TRUE(outcome.mIsSlovenian);
    EXPECT_TRUE(outcome.mSlovenianTaxNumberMissing);
}

TEST(IncomePayersTest, ASlovenianPayerNumberOfEightZerosIsNotWritten) {
    // A tax number of eight zeros is not valid.
    const PayerEntry entry{
        .mIsin = "SI0000000001",
        .mIsinSource = "SI0000000001",
        .mName = "Example Izdajatelj d.d.",
        .mTaxNumber = "00000000",
        .mAddress = "1 Main St, Ljubljana",
        .mCountry = "SI",
        .mInstrumentKind = std::nullopt,
        .mVerifiedOn = std::nullopt,
        .mSource = {.mSource = "manual", .mFile = "payers.csv", .mRowNumber = 2},
    };

    const auto outcome = payerOutcomeOf(entry);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mName, std::optional<std::string>{"Example Izdajatelj d.d."});
    EXPECT_TRUE(outcome.mIsSlovenian);
    EXPECT_TRUE(outcome.mSlovenianTaxNumberMissing);
}

TEST(IncomePayersTest, ASlovenianPayerNumberWithCheckDigitTenIsZero) {
    // 1000002: 8 + 4 = 12, 12 mod 11 = 1, 11 - 1 = 10, written as 0.
    const PayerEntry entry{
        .mIsin = "SI0000000001",
        .mIsinSource = "SI0000000001",
        .mName = "Example Izdajatelj d.d.",
        .mTaxNumber = "10000020",
        .mAddress = "1 Main St, Ljubljana",
        .mCountry = "SI",
        .mInstrumentKind = std::nullopt,
        .mVerifiedOn = std::nullopt,
        .mSource = {.mSource = "manual", .mFile = "payers.csv", .mRowNumber = 2},
    };

    const auto outcome = payerOutcomeOf(entry);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::optional<std::string>{"10000020"});
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mCountry, std::optional<std::string>{"SI"});
    EXPECT_TRUE(outcome.mIsSlovenian);
    EXPECT_FALSE(outcome.mSlovenianTaxNumberMissing);
}

TEST(IncomePayersTest, ASlovenianPayerNumberWithRemainderZeroIsNotWritten) {
    // 1000010: 8 + 3 = 11, a multiple of 11, so no check digit fits.
    const PayerEntry entry{
        .mIsin = "SI0000000001",
        .mIsinSource = "SI0000000001",
        .mName = "Example Izdajatelj d.d.",
        .mTaxNumber = "10000100",
        .mAddress = "1 Main St, Ljubljana",
        .mCountry = "SI",
        .mInstrumentKind = std::nullopt,
        .mVerifiedOn = std::nullopt,
        .mSource = {.mSource = "manual", .mFile = "payers.csv", .mRowNumber = 2},
    };

    const auto outcome = payerOutcomeOf(entry);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mName, std::optional<std::string>{"Example Izdajatelj d.d."});
    EXPECT_TRUE(outcome.mIsSlovenian);
    EXPECT_TRUE(outcome.mSlovenianTaxNumberMissing);
}

TEST(IncomePayersTest, ASlovenianPayerWithoutANumberHasNeitherNumber) {
    // No number in the reference row: nothing is written, and the number is missing.
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
    EXPECT_TRUE(outcome.mSlovenianTaxNumberMissing);
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
    EXPECT_FALSE(outcome.mSlovenianTaxNumberMissing);
}

TEST(IncomePayersTest, ASlovenianBrokerNumberBecomesTheTaxNumber) {
    // A Slovenian broker company has the same rule as a Slovenian issuer.
    const BrokerPayerEntry entry{
        .mBroker = "SYN",
        .mCode = "SYN",
        .mName = "Example Broker d.o.o.",
        .mTaxNumber = "12345679",
        .mAddress = "1 Main St, Ljubljana",
        .mCountry = "SI",
        .mSource = {.mSource = "manual", .mFile = "broker_payers.csv", .mRowNumber = 2},
    };

    const auto outcome = payerOutcomeOf(entry);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::optional<std::string>{"12345679"});
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mCountry, std::optional<std::string>{"SI"});
    EXPECT_TRUE(outcome.mIsSlovenian);
    EXPECT_FALSE(outcome.mSlovenianTaxNumberMissing);
}

TEST(IncomePayersTest, ASlovenianBrokerNumberWithThePrefixIsNotATaxNumber) {
    // `SI` and the digits is not a tax number.
    const BrokerPayerEntry entry{
        .mBroker = "SYN",
        .mCode = "SYN",
        .mName = "Example Broker d.o.o.",
        .mTaxNumber = "SI12345679",
        .mAddress = "1 Main St, Ljubljana",
        .mCountry = "SI",
        .mSource = {.mSource = "manual", .mFile = "broker_payers.csv", .mRowNumber = 2},
    };

    const auto outcome = payerOutcomeOf(entry);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mIdentificationNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mName, std::optional<std::string>{"Example Broker d.o.o."});
    EXPECT_TRUE(outcome.mIsSlovenian);
    EXPECT_TRUE(outcome.mSlovenianTaxNumberMissing);
}

/// A catalogue with one Slovenian payer whose tax number has the wrong check digit.
ReferenceCatalogue catalogueWithSlovenianPayerWithoutNumber() {
    const std::string payers =
        "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
        "source_revision\n"
        "XX0000000010,Issuer A,12345674,1 Main Street,SI,,,issuer,manual,\n";
    const std::string relief = "country,income,statement,source,source_revision\n"
                               "BE,dividend,Treaty text,manual,\n";
    const std::string broker =
        "broker,code,name,tax_number,address,country,source,source_revision\n"
        "TR,TR-X,Broker T,TAX-T,\"1 Main Street, Berlin\",DE,manual,\n";

    return parseReferenceCatalogue(payers, relief, broker);
}

TEST(IncomePayersTest, AFormThatReportsASlovenianPayerWarnsWhenItsNumberIsMissing) {
    // The form has no Slovenian exclusion (like the coupon form), so the row is written and the
    // user must enter the tax number in eDavki: one warning, and the payer stays Slovenian-less.
    const auto catalogue = catalogueWithSlovenianPayerWithoutNumber();
    const PayerForm form{.mReport = ReportType::OtherInterest,
                         .mNotFoundCode = ProcessingDiagnosticCode::InterestPayerNotFound,
                         .mNotFoundKey = MessageKey::InterestPayerNotFound,
                         .mSlovenianExclusion = std::nullopt};
    std::vector<ProcessingDiagnostic> diagnostics;
    std::vector<RequestInput> requests;
    std::vector<ExcludedItemInput> excluded;
    const PayerSinks sinks{diagnostics, requests, excluded};
    const IsinPayments payments{.mName = "Issuer A", .mSources = {}, .mFirstDate = Date{}};
    RunDecisions decisions;

    const auto outcome = resolvePayer(form, sinks, "XX0000000010", payments, catalogue, decisions);

    EXPECT_EQ(outcome.mPayer.mTaxNumber, std::nullopt);
    EXPECT_EQ(outcome.mPayer.mName, std::optional<std::string>{"Issuer A"});
    EXPECT_FALSE(outcome.mIsSlovenian);
    EXPECT_TRUE(outcome.mSlovenianTaxNumberMissing);

    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.at(0).mCode, ProcessingDiagnosticCode::SlovenianPayerTaxNumberMissing);
    EXPECT_EQ(diagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_TRUE(excluded.empty());
}

TEST(IncomePayersTest, AFormThatExcludesASlovenianPayerDoesNotWarnAboutItsNumber) {
    // The dividend form leaves a Slovenian payer out (it settles the tax itself): no row is
    // written, so a missing tax number needs no warning.
    const auto catalogue = catalogueWithSlovenianPayerWithoutNumber();
    const PayerForm form{.mReport = ReportType::Dividends,
                         .mNotFoundCode = ProcessingDiagnosticCode::DividendPayerNotFound,
                         .mNotFoundKey = MessageKey::DividendPayerNotFound,
                         .mSlovenianExclusion = ExcludedCategory::Dividend};
    std::vector<ProcessingDiagnostic> diagnostics;
    std::vector<RequestInput> requests;
    std::vector<ExcludedItemInput> excluded;
    const PayerSinks sinks{diagnostics, requests, excluded};
    const IsinPayments payments{.mName = "Issuer A", .mSources = {}, .mFirstDate = Date{}};
    RunDecisions decisions;

    const auto outcome = resolvePayer(form, sinks, "XX0000000010", payments, catalogue, decisions);

    EXPECT_TRUE(outcome.mIsSlovenian);
    EXPECT_TRUE(diagnostics.empty());
}

} // namespace
