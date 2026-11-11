#include "utils/date_utils.hpp"
#include "xml/report_xml_stage.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;

constexpr auto kCapital = ReportType::CapitalGains;
constexpr auto kDividends = ReportType::Dividends;
constexpr auto kBank = ReportType::BankDepositInterest;
constexpr auto kOther = ReportType::OtherInterest;

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

const ReportMetadata kMetadata{
    .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
    .mReportingYear = 2025,
    .mFilingType = FilingType::Original,
};

// The four valid models. Each test that needs a broken form writes that model in full.

/// One sale that uses one purchase, 10 units bought at EUR 10 and sold at EUR 12.
const CapitalGainsReportKDVP kValidCapital{
    .mLists = {{
        .mIsin = "XX0000000001",
        .mName = "Example Shares",
        .mIsFund = false,
        .mRows =
            {
                {.mEntry =
                     KdvpPurchaseRow{
                         .mPurchase = {StatementEventKind::Trade, 0, 0},
                         .mDate = day("2025-01-10"),
                         .mMethod = KdvpPurchaseMethod::B,
                         .mUnits = 1'000'000'000,     // 10 units
                         .mUnitPrice = 1'000'000'000, // EUR 10
                     },
                 .mStock = 1'000'000'000}, // 10
                {.mEntry =
                     KdvpSaleRow{
                         .mSale = {StatementEventKind::Trade, 0, 1},
                         .mDate = day("2025-03-10"),
                         .mUnits = 1'000'000'000,     // 10 units
                         .mUnitPrice = 1'200'000'000, // EUR 12
                         .mLossRule = KdvpLossRule::NotBlocked,
                     },
                 .mStock = 0}, // 10 - 10
            },
    }}};

/// One dividend of EUR 100.00 with EUR 15.00 foreign tax.
const DividendReportDiv kValidDividends{
    .mRows = {{
        .mPayment = {StatementEventKind::Dividend, 0, 0},
        .mIsin = "US0000000001",
        .mDate = day("2025-03-14"),
        .mPayer = {.mName = "Example Corp",
                   .mAddress = "1 Main St, Springfield",
                   .mCountry = "US",
                   .mIdentificationNumber = "12-3456789"},
        .mKind = DividendKind::Ordinary,
        .mGrossIncome = 1'000'000,                                         // EUR 100.00
        .mWithholding = {.mForeignTax = 150'000, .mBlankInExport = false}, // EUR 15.00
        .mSourceCountry = "US",
        .mTreatyClaim = std::nullopt,
    }}};

/// Bank interest of EUR 50.00 without tax.
const DepositInterestReportDHO kValidDeposit{
    .mRows = {{
        .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
        .mPayer = {.mName = "Example Bank AG",
                   .mAddress = "1 Example St, 10115 Berlin",
                   .mCountry = "DE",
                   .mIdentificationNumber = "DE123456789"},
        .mKind = DepositInterestKind::BankDeposit,
        .mInterest = 500'000, // EUR 50.00
        .mForeignTax = 0,
        .mSourceCountry = "DE",
    }}};

/// Broker cash interest of EUR 12.34 and a coupon of EUR 25.00 with EUR 3.75 foreign tax.
const OtherInterestReportObr kValidOther{
    .mRows = {
        {
            .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
            .mKind = BrokerCashInterest{.mBroker = Broker::TradeRepublic},
            .mDate = day("2025-03-31"),
            .mPayer = {.mName = "Example Broker AG",
                       .mAddress = "Example Street 1, 10115 Berlin",
                       .mCountry = "DE",
                       .mIdentificationNumber = "DE000000000"},
            .mGrossInterest = 123'400,
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
            .mSourceCountry = "DE",
            .mTreatyClaim = std::nullopt,
        },
        {
            .mPayment = {StatementEventKind::Interest, std::size_t{1}, std::size_t{0}},
            .mKind = BondCoupon{.mIsin = "FR0000000001"},
            .mDate = day("2025-06-15"),
            .mPayer = {.mName = "Example Issuer SA",
                       .mAddress = "1 Rue Exemple, 75001 Paris",
                       .mCountry = "FR",
                       .mIdentificationNumber = "FR0000000000"},
            .mGrossInterest = 250'000,
            .mWithholding = {.mForeignTax = 37'500, .mBlankInExport = false},
            .mSourceCountry = "FR",
            .mTreatyClaim = std::nullopt,
        },
    }};

/// The one error of an XML writer: its code and form, and no field when aField is empty.
void expectOneError(const ApplicationResult& aResult,
                    ReportType aReport,
                    ProcessingDiagnosticCode aCode,
                    std::string_view aField) {
    const auto& ids = aResult.report(aReport).mDiagnosticIds;

    ASSERT_EQ(ids.size(), 1U);

    const auto found = std::find_if(aResult.mDiagnostics.begin(),
                                    aResult.mDiagnostics.end(),
                                    [&ids](const ApplicationDiagnostic& aDiagnostic) {
                                        return aDiagnostic.mId == ids.front();
                                    });

    ASSERT_NE(found, aResult.mDiagnostics.end());
    EXPECT_EQ(found->mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(found->mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(found->mAffectedReports, std::vector{aReport});
    EXPECT_EQ(found->mField.value_or(""), aField);
    EXPECT_EQ(found->mCode, ApplicationDiagnosticCode{aCode});
}

TEST(ReportXmlStageTest, AllFourFormsAreWritten) {
    ApplicationResultInput input{.mReportingYear = 2025};

    writeReportXml(input,
                   kMetadata,
                   {.mCapitalGains = &kValidCapital,
                    .mDividends = &kValidDividends,
                    .mDeposit = &kValidDeposit,
                    .mOther = &kValidOther});
    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::Success);
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::Generated);

    // Each XML is its own form: the dividend gross EUR 100.00 is written as 100.00 in Doh-Div.
    EXPECT_NE(result.report(kCapital).mXml->find("Doh_KDVP_9.xsd"), std::string::npos);
    EXPECT_NE(result.report(kDividends).mXml->find("<Value>100.00</Value>"), std::string::npos);
    EXPECT_NE(result.report(kBank).mXml->find("<InterestEarned>50.00</InterestEarned>"),
              std::string::npos);
    EXPECT_NE(result.report(kOther).mXml->find("Doh_Obr_2.xsd"), std::string::npos);
}

TEST(ReportXmlStageTest, OnlyCapitalGainsFailsOnAFieldOutOfRange) {
    ApplicationResultInput input{.mReportingYear = 2025};
    // The ISIN has 13 characters; the form needs 12.
    const CapitalGainsReportKDVP brokenCapital{
        .mLists = {{
            .mIsin = "XX00000000011",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2025-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 1'000'000'000,     // 10 units
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 1'000'000'000}, // 10
                },
        }}};

    writeReportXml(input,
                   kMetadata,
                   {.mCapitalGains = &brokenCapital,
                    .mDividends = &kValidDividends,
                    .mDeposit = &kValidDeposit,
                    .mOther = &kValidOther});
    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(result.report(kCapital).mXml.has_value());
    expectOneError(result, kCapital, ProcessingDiagnosticCode::XmlValueOutOfRange, "ISIN");

    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::Generated);
}

TEST(ReportXmlStageTest, OnlyDividendsFailsOnAFieldOutOfRange) {
    ApplicationResultInput input{.mReportingYear = 2025};
    // The payer country "S1" is not two uppercase letters.
    const DividendReportDiv brokenDividends{
        .mRows = {{
            .mPayment = {StatementEventKind::Dividend, 0, 0},
            .mIsin = "US0000000001",
            .mDate = day("2025-03-14"),
            .mPayer = {.mName = "Example Corp",
                       .mAddress = "1 Main St, Springfield",
                       .mCountry = "S1",
                       .mIdentificationNumber = "12-3456789"},
            .mKind = DividendKind::Ordinary,
            .mGrossIncome = 1'000'000,
            .mWithholding = {.mForeignTax = 150'000, .mBlankInExport = false},
            .mSourceCountry = "US",
            .mTreatyClaim = std::nullopt,
        }}};

    writeReportXml(input,
                   kMetadata,
                   {.mCapitalGains = &kValidCapital,
                    .mDividends = &brokenDividends,
                    .mDeposit = &kValidDeposit,
                    .mOther = &kValidOther});
    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(result.report(kDividends).mXml.has_value());
    expectOneError(result,
                   kDividends,
                   ProcessingDiagnosticCode::XmlValueOutOfRange,
                   "PayerCountry");

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::Generated);
}

TEST(ReportXmlStageTest, OnlyBankInterestFailsOnAFieldOutOfRange) {
    ApplicationResultInput input{.mReportingYear = 2025};
    // The payer country "si" is lowercase.
    const DepositInterestReportDHO brokenDeposit{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "Example Bank AG",
                       .mAddress = "1 Example St, 10115 Berlin",
                       .mCountry = "si",
                       .mIdentificationNumber = "DE123456789"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 500'000,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    writeReportXml(input,
                   kMetadata,
                   {.mCapitalGains = &kValidCapital,
                    .mDividends = &kValidDividends,
                    .mDeposit = &brokenDeposit,
                    .mOther = &kValidOther});
    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(result.report(kBank).mXml.has_value());
    expectOneError(result, kBank, ProcessingDiagnosticCode::XmlValueOutOfRange, "PayerCountryCode");

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::Generated);
}

TEST(ReportXmlStageTest, OtherInterestFailsAsOneFormWithBothSubtypes) {
    ApplicationResultInput input{.mReportingYear = 2025};
    // Cash interest and a coupon in one form; the coupon's source country "X" has one letter,
    // so the one document is not written and the valid cash interest row is not either.
    const OtherInterestReportObr brokenOther{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{.mBroker = Broker::TradeRepublic},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "Example Broker AG",
                           .mAddress = "Example Street 1, 10115 Berlin",
                           .mCountry = "DE",
                           .mIdentificationNumber = "DE000000000"},
                .mGrossInterest = 123'400,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{1}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "FR0000000001"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "Example Issuer SA",
                           .mAddress = "1 Rue Exemple, 75001 Paris",
                           .mCountry = "FR",
                           .mIdentificationNumber = "FR0000000000"},
                .mGrossInterest = 250'000,
                .mWithholding = {.mForeignTax = 37'500, .mBlankInExport = false},
                .mSourceCountry = "X",
                .mTreatyClaim = std::nullopt,
            },
        }};

    writeReportXml(input,
                   kMetadata,
                   {.mCapitalGains = &kValidCapital,
                    .mDividends = &kValidDividends,
                    .mDeposit = &kValidDeposit,
                    .mOther = &brokenOther});
    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(result.report(kOther).mXml.has_value());
    expectOneError(result, kOther, ProcessingDiagnosticCode::XmlValueOutOfRange, "Country2");

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::Generated);
}

TEST(ReportXmlStageTest, AThrowingDividendWriterFailsOnlyDividends) {
    ApplicationResultInput input{.mReportingYear = 2025};
    // Kind 99 is no dividend kind, so the writer throws.
    const DividendReportDiv brokenDividends{
        .mRows = {{
            .mPayment = {StatementEventKind::Dividend, 0, 0},
            .mIsin = "US0000000001",
            .mDate = day("2025-03-14"),
            .mPayer = {.mName = "Example Corp",
                       .mAddress = "1 Main St, Springfield",
                       .mCountry = "US",
                       .mIdentificationNumber = "12-3456789"},
            .mKind = static_cast<DividendKind>(99),
            .mGrossIncome = 1'000'000,
            .mWithholding = {.mForeignTax = 150'000, .mBlankInExport = false},
            .mSourceCountry = "US",
            .mTreatyClaim = std::nullopt,
        }}};

    writeReportXml(input,
                   kMetadata,
                   {.mCapitalGains = &kValidCapital,
                    .mDividends = &brokenDividends,
                    .mDeposit = &kValidDeposit,
                    .mOther = &kValidOther});
    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(result.report(kDividends).mXml.has_value());
    expectOneError(result, kDividends, ProcessingDiagnosticCode::XmlSerializationFailed, "");

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::XmlSerializationFailed);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_FALSE(diagnostic.mIsin.has_value());
    // The exception text goes to the log only, never to the user.
    EXPECT_EQ(diagnostic.mMessage.find("Unknown dividend kind"), std::string::npos);

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::Generated);
}

TEST(ReportXmlStageTest, AThrowingDepositWriterFailsOnlyBankInterest) {
    ApplicationResultInput input{.mReportingYear = 2025};
    // Kind 99 is no deposit interest kind, so the writer throws.
    const DepositInterestReportDHO brokenDeposit{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "Example Bank AG",
                       .mAddress = "1 Example St, 10115 Berlin",
                       .mCountry = "DE",
                       .mIdentificationNumber = "DE123456789"},
            .mKind = static_cast<DepositInterestKind>(99),
            .mInterest = 500'000,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    writeReportXml(input,
                   kMetadata,
                   {.mCapitalGains = &kValidCapital,
                    .mDividends = &kValidDividends,
                    .mDeposit = &brokenDeposit,
                    .mOther = &kValidOther});
    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(result.report(kBank).mXml.has_value());
    expectOneError(result, kBank, ProcessingDiagnosticCode::XmlSerializationFailed, "");

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::Generated);
}

TEST(ReportXmlStageTest, AnUnknownFilingTypeFailsOnlyDohKdvp) {
    ApplicationResultInput input{.mReportingYear = 2025};
    // Filing type 9 is neither an original nor a self-report. Only Doh-KDVP writes the filing
    // type; the three income forms are always an original, so they do not read it.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = static_cast<FilingType>(9),
    };

    writeReportXml(input,
                   metadata,
                   {.mCapitalGains = &kValidCapital,
                    .mDividends = &kValidDividends,
                    .mDeposit = &kValidDeposit,
                    .mOther = &kValidOther});
    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(result.report(kCapital).mXml.has_value());
    expectOneError(result, kCapital, ProcessingDiagnosticCode::XmlSerializationFailed, "");

    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::Generated);
}

TEST(ReportXmlStageTest, AWriterThatThrowsSomethingElseFailsOnlyItsForm) {
    ApplicationResultInput input{.mReportingYear = 2025};

    // A capital XML that an earlier writer already stored.
    input.mReports[static_cast<std::size_t>(kCapital)].mXml = "<capital/>";

    // The writer throws an int, which is not a standard exception.
    writeGuardedXml(input, kOther, "Doh-Obr", []() -> XmlWriteResult { throw 42; });
    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(result.report(kOther).mXml.has_value());
    expectOneError(result, kOther, ProcessingDiagnosticCode::XmlSerializationFailed, "");

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
}

TEST(ReportXmlStageTest, AnAbsentOrEmptyModelWritesNothing) {
    ApplicationResultInput input{.mReportingYear = 2025};
    // The dividend model exists but has no rows; the other two income models are absent.
    const DividendReportDiv emptyDividends{.mRows = {}};

    writeReportXml(input,
                   kMetadata,
                   {.mCapitalGains = &kValidCapital, .mDividends = &emptyDividends});
    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::Success);
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::NoData);
}

} // namespace
