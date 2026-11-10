#include "../support/xsd_validation.hpp"
#include "xml/dho_xml_writer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;
using taxbroker::test::expectInvalidForDhoSchema;
using taxbroker::test::expectValidForDhoSchema;

/// The texts of all elements with this name, in document order.
std::vector<std::string> textsOf(const std::string& aXml, std::string_view aElement) {
    const std::string open = "<" + std::string{aElement} + ">";
    const std::string close = "</" + std::string{aElement} + ">";
    std::vector<std::string> texts;
    std::size_t position = 0;

    while ((position = aXml.find(open, position)) != std::string::npos)
    {
        const std::size_t start = position + open.size();
        const std::size_t end = aXml.find(close, start);

        texts.push_back(aXml.substr(start, end - start));
        position = end;
    }

    return texts;
}

/// One error, no XML: the field, the report and the payer name (empty when none is valid text).
void expectOneError(const XmlWriteResult& aResult,
                    std::string_view aField,
                    std::optional<std::string_view> aName) {
    EXPECT_FALSE(aResult.mXml.has_value());
    ASSERT_EQ(aResult.mDiagnostics.size(), 1U);

    const auto& diagnostic = aResult.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports, std::vector{ReportType::BankDepositInterest});
    EXPECT_EQ(diagnostic.mContext.mField, aField);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", std::string{aField})});
    EXPECT_FALSE(diagnostic.mContext.mIsin.has_value());

    if (aName)
    {
        EXPECT_EQ(diagnostic.mContext.mInstrumentName, std::string{*aName});
    }
    else
    {
        EXPECT_FALSE(diagnostic.mContext.mInstrumentName.has_value());
    }
}

TEST(DhoXmlWriterTest, OneRowWritesTheWholeDocument) {
    // Interest EUR 1234.5650 is 1234.57 (half cent up); the tax is 0.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer =
                {
                    .mName = "Example Bank AG",
                    .mAddress = "1 Example St, 10115 Berlin",
                    .mCountry = "DE",
                    .mIdentificationNumber = "DE123456789",
                },
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 12'345'650,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_DHO_4.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
      <edp:resident>true</edp:resident>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_DHO>
      <Doh_DHO_TaxPayerData>
        <SelfReport>false</SelfReport>
        <IsResident>true</IsResident>
      </Doh_DHO_TaxPayerData>
      <Doh_DHO_InterestEarned>
        <IDeu>DE123456789</IDeu>
        <Title>Example Bank AG</Title>
        <Address>1 Example St, 10115 Berlin</Address>
        <PayerCountryCode>DE</PayerCountryCode>
        <InterestType>1</InterestType>
        <InterestEarned>1234.57</InterestEarned>
        <ForeignTaxPaid>0.00</ForeignTaxPaid>
        <SourceCountryCode>DE</SourceCountryCode>
      </Doh_DHO_InterestEarned>
      <Doh_DHO_Totals>
        <TotalInterestEarned>1234.57</TotalInterestEarned>
        <TotalForeignTaxPaid>0.00</TotalForeignTaxPaid>
        <Year>2025</Year>
      </Doh_DHO_Totals>
    </Doh_DHO>
  </body>
</Envelope>
)xml");
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, TaxpayerContactFieldsAreWrittenInSchemaOrder) {
    const ReportMetadata metadata{
        .mTaxpayer =
            {
                .mTaxNumber = "12345678",
                .mIsResident = true,
                .mEmail = "taxpayer@example.com",
                .mPhoneNumber = "+38600000000",
            },
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{.mRows = {}};

    const auto result = DhoXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // Doh_DHO_TaxPayerData order: Email, PhoneNumber, SelfReport, IsResident.
    EXPECT_NE(result.mXml->find("<Doh_DHO_TaxPayerData>\n"
                                "        <Email>taxpayer@example.com</Email>\n"
                                "        <PhoneNumber>+38600000000</PhoneNumber>\n"
                                "        <SelfReport>false</SelfReport>\n"
                                "        <IsResident>true</IsResident>\n"
                                "      </Doh_DHO_TaxPayerData>"),
              std::string::npos);
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, SelfReportWritesItsWorkflowCode) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::SelfReport,
    };
    const DepositInterestReportDHO report{.mRows = {}};

    const auto result = DhoXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "edp:DocumentWorkflowID"), std::vector<std::string>{"I"});
    EXPECT_EQ(textsOf(*result.mXml, "edp:DocumentWorkflowName"),
              std::vector<std::string>{"SamoPrijava"});
    EXPECT_EQ(textsOf(*result.mXml, "SelfReport"), std::vector<std::string>{"true"});
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, PayerNotFoundLeavesThePayerFieldsOut) {
    // No payer facts and no source country: only the type, the amounts and the tax are written.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 5'000'000,
            .mForeignTax = 0,
            .mSourceCountry = std::nullopt,
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_NE(result.mXml->find("      <Doh_DHO_InterestEarned>\n"
                                "        <InterestType>1</InterestType>\n"
                                "        <InterestEarned>500.00</InterestEarned>\n"
                                "        <ForeignTaxPaid>0.00</ForeignTaxPaid>\n"
                                "      </Doh_DHO_InterestEarned>"),
              std::string::npos);
    EXPECT_TRUE(textsOf(*result.mXml, "Title").empty());
    EXPECT_TRUE(textsOf(*result.mXml, "IDeu").empty());
    EXPECT_TRUE(textsOf(*result.mXml, "PayerCountryCode").empty());
    EXPECT_TRUE(textsOf(*result.mXml, "SourceCountryCode").empty());
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, SlovenianPayerWritesTaxIdAndNoForeignTax) {
    // The form wants the foreign tax empty for a Slovenian payer; the total of the tax is 0.00.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer =
                {
                    .mName = "Example Bank d.d.",
                    .mAddress = "1 Main St, Ljubljana",
                    .mCountry = "SI",
                    .mTaxNumber = "87654321",
                },
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 5'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "SI",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_NE(result.mXml->find("      <Doh_DHO_InterestEarned>\n"
                                "        <TaxID>87654321</TaxID>\n"
                                "        <Title>Example Bank d.d.</Title>\n"
                                "        <Address>1 Main St, Ljubljana</Address>\n"
                                "        <PayerCountryCode>SI</PayerCountryCode>\n"
                                "        <InterestType>1</InterestType>\n"
                                "        <InterestEarned>500.00</InterestEarned>\n"
                                "        <SourceCountryCode>SI</SourceCountryCode>\n"
                                "      </Doh_DHO_InterestEarned>"),
              std::string::npos);
    EXPECT_TRUE(textsOf(*result.mXml, "ForeignTaxPaid").empty());
    EXPECT_EQ(textsOf(*result.mXml, "TotalForeignTaxPaid"), std::vector<std::string>{"0.00"});
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, SlovenianPayerWithATaxThatRoundsToZeroIsAccepted) {
    // Tax EUR 0.0040 is 0.00 after rounding, so nothing is left to leave out wrongly.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "Example Bank d.d.", .mCountry = "SI"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 5'000'000,
            .mForeignTax = 40,
            .mSourceCountry = "SI",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_TRUE(textsOf(*result.mXml, "ForeignTaxPaid").empty());
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, SlovenianPayerWithAForeignTaxStopsTheFile) {
    // Tax EUR 1.0000 for a Slovenian payer: the form wants the field empty.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "Example Bank d.d.", .mCountry = "SI"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 5'000'000,
            .mForeignTax = 10'000,
            .mSourceCountry = "SI",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "ForeignTaxPaid", "Example Bank d.d.");
}

TEST(DhoXmlWriterTest, HalfCentsRoundAwayFromZeroOnce) {
    // Row 1: interest 10.0049 is 10.00, tax 0.0050 is 0.01.
    // Row 2: interest 10.0050 is 10.01, tax 0.0049 is 0.00.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {
            {
                .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
                .mPayer = {.mName = "A Bank", .mCountry = "DE"},
                .mKind = DepositInterestKind::BankDeposit,
                .mInterest = 100'049,
                .mForeignTax = 50,
                .mSourceCountry = "DE",
            },
            {
                .mPayments = {{StatementEventKind::Interest, std::size_t{1}, std::size_t{0}}},
                .mPayer = {.mName = "B Bank", .mCountry = "DE"},
                .mKind = DepositInterestKind::BankDeposit,
                .mInterest = 100'050,
                .mForeignTax = 49,
                .mSourceCountry = "DE",
            },
        }};

    const auto result = DhoXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "InterestEarned"),
              (std::vector<std::string>{"10.00", "10.01"}));
    EXPECT_EQ(textsOf(*result.mXml, "ForeignTaxPaid"), (std::vector<std::string>{"0.01", "0.00"}));
    // Totals are the sums of the written rows: 10.00 + 10.01 and 0.01 + 0.00.
    EXPECT_EQ(textsOf(*result.mXml, "TotalInterestEarned"), std::vector<std::string>{"20.01"});
    EXPECT_EQ(textsOf(*result.mXml, "TotalForeignTaxPaid"), std::vector<std::string>{"0.01"});
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, TotalsAreTheSumOfTheRoundedRows) {
    // Two rows of 0.0050 are 0.01 each and 0.02 together; rounding the exact sum 0.0100 first
    // would give 0.01 and disagree with the rows.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {
            {
                .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
                .mPayer = {.mName = "A Bank", .mCountry = "DE"},
                .mKind = DepositInterestKind::BankDeposit,
                .mInterest = 50,
                .mForeignTax = 50,
                .mSourceCountry = "DE",
            },
            {
                .mPayments = {{StatementEventKind::Interest, std::size_t{1}, std::size_t{0}}},
                .mPayer = {.mName = "B Bank", .mCountry = "DE"},
                .mKind = DepositInterestKind::BankDeposit,
                .mInterest = 50,
                .mForeignTax = 50,
                .mSourceCountry = "DE",
            },
        }};

    const auto result = DhoXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "InterestEarned"), (std::vector<std::string>{"0.01", "0.01"}));
    EXPECT_EQ(textsOf(*result.mXml, "TotalInterestEarned"), std::vector<std::string>{"0.02"});
    EXPECT_EQ(textsOf(*result.mXml, "TotalForeignTaxPaid"), std::vector<std::string>{"0.02"});
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, RowsKeepTheModelOrder) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {
            {
                .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
                .mPayer = {.mName = "A Bank", .mCountry = "DE"},
                .mKind = DepositInterestKind::BankDeposit,
                .mInterest = 1'000'000,
                .mForeignTax = 0,
                .mSourceCountry = "DE",
            },
            {
                .mPayments = {{StatementEventKind::Interest, std::size_t{1}, std::size_t{0}}},
                .mPayer = {.mName = "B Bank", .mCountry = "DE"},
                .mKind = DepositInterestKind::BankDeposit,
                .mInterest = 2'000'000,
                .mForeignTax = 0,
                .mSourceCountry = "DE",
            },
        }};

    const auto result = DhoXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "Title"), (std::vector<std::string>{"A Bank", "B Bank"}));
    EXPECT_EQ(textsOf(*result.mXml, "InterestEarned"),
              (std::vector<std::string>{"100.00", "200.00"}));
    EXPECT_EQ(textsOf(*result.mXml, "TotalInterestEarned"), std::vector<std::string>{"300.00"});
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, TextIsEscapedAndNonAsciiIsKept) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A & B <Bank>", .mAddress = "Šparkasse, Dunaj", .mCountry = "AT"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "AT",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_NE(result.mXml->find("<Title>A &amp; B &lt;Bank&gt;</Title>"), std::string::npos);
    EXPECT_NE(result.mXml->find("<Address>Šparkasse, Dunaj</Address>"), std::string::npos);
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, AnEmptyReportWritesNoInterestRowAndZeroTotals) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{.mRows = {}};

    const auto result = DhoXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(result.mXml->find("Doh_DHO_InterestEarned"), std::string::npos);
    EXPECT_EQ(textsOf(*result.mXml, "TotalInterestEarned"), std::vector<std::string>{"0.00"});
    EXPECT_EQ(textsOf(*result.mXml, "TotalForeignTaxPaid"), std::vector<std::string>{"0.00"});
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, SchemaRejectsAnAmountWithThreeDecimals) {
    // Proves that the schema check can fail: InterestEarned has two decimals at most.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_DHO_4.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_DHO>
      <Doh_DHO_InterestEarned>
        <InterestEarned>1.234</InterestEarned>
      </Doh_DHO_InterestEarned>
      <Doh_DHO_Totals>
        <Year>2025</Year>
      </Doh_DHO_Totals>
    </Doh_DHO>
  </body>
</Envelope>
)xml";

    expectInvalidForDhoSchema(xml);
}

TEST(DhoXmlWriterTest, LargestAmountThatFitsTheSchemaIsWritten) {
    // EUR 9999999999.9949 is 9999999999.99, the largest value of 12 digits with 2 decimals.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 99'999'999'999'949,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "InterestEarned"), std::vector<std::string>{"9999999999.99"});
    EXPECT_EQ(textsOf(*result.mXml, "TotalInterestEarned"),
              std::vector<std::string>{"9999999999.99"});
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, AnAmountThatRoundsAboveTheSchemaLimitIsRejected) {
    // EUR 9999999999.9950 rounds up to 10000000000.00, which has 13 digits.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 99'999'999'999'950,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "InterestEarned", "A Bank");
}

TEST(DhoXmlWriterTest, AForeignTaxAboveTheSchemaLimitIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 99'999'999'999'950,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "ForeignTaxPaid", "A Bank");
}

TEST(DhoXmlWriterTest, TotalAboveTheSchemaLimitIsRejectedEvenWhenEveryRowFits) {
    // Two rows of EUR 6000000000.00 fit; their sum 12000000000.00 has 14 digits.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {
            {
                .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
                .mPayer = {.mName = "A Bank", .mCountry = "DE"},
                .mKind = DepositInterestKind::BankDeposit,
                .mInterest = 60'000'000'000'000,
                .mForeignTax = 0,
                .mSourceCountry = "DE",
            },
            {
                .mPayments = {{StatementEventKind::Interest, std::size_t{1}, std::size_t{0}}},
                .mPayer = {.mName = "B Bank", .mCountry = "DE"},
                .mKind = DepositInterestKind::BankDeposit,
                .mInterest = 60'000'000'000'000,
                .mForeignTax = 0,
                .mSourceCountry = "DE",
            },
        }};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "TotalInterestEarned", std::nullopt);
}

TEST(DhoXmlWriterTest, ForeignTaxTotalAboveTheSchemaLimitIsRejectedEvenWhenEveryRowFits) {
    // Two taxes of EUR 6000000000.00 fit; their sum 12000000000.00 has 14 digits.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {
            {
                .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
                .mPayer = {.mName = "A Bank", .mCountry = "DE"},
                .mKind = DepositInterestKind::BankDeposit,
                .mInterest = 1'000'000,
                .mForeignTax = 60'000'000'000'000,
                .mSourceCountry = "DE",
            },
            {
                .mPayments = {{StatementEventKind::Interest, std::size_t{1}, std::size_t{0}}},
                .mPayer = {.mName = "B Bank", .mCountry = "DE"},
                .mKind = DepositInterestKind::BankDeposit,
                .mInterest = 1'000'000,
                .mForeignTax = 60'000'000'000'000,
                .mSourceCountry = "DE",
            },
        }};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "TotalForeignTaxPaid", std::nullopt);
}

TEST(DhoXmlWriterTest, TitleOf50CharactersIsAccepted) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const std::string title(50, 'A');
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = title, .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "Title"), std::vector<std::string>{title});
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, TitleOf50TwoByteCharactersIsAcceptedBecauseCharactersAreCounted) {
    // 50 times "Š" is 50 characters and 100 bytes.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    std::string title;

    for (int count = 0; count < 50; ++count)
    {
        title += "Š";
    }

    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = title, .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, TitleOf51CharactersIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const std::string title(51, 'A');
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = title, .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "Title", title);
}

TEST(DhoXmlWriterTest, EmptyTitleIsRejected) {
    // The schema wants at least one character; a missing name is left out instead.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "", .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "Title", std::nullopt);
}

TEST(DhoXmlWriterTest, AddressOf101CharactersIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const std::string address(101, 'B');
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mAddress = address, .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "Address", "A Bank");
}

TEST(DhoXmlWriterTest, InterestOfZeroIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 0,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "InterestEarned", "A Bank");
}

TEST(DhoXmlWriterTest, InterestThatRoundsToZeroIsRejected) {
    // EUR 0.0049 is 0.00.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 49,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "InterestEarned", "A Bank");
}

TEST(DhoXmlWriterTest, NegativeInterestIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = -10'000,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "InterestEarned", "A Bank");
}

TEST(DhoXmlWriterTest, NegativeForeignTaxIsRejected) {
    // Tax EUR -0.0001 would round to 0.00, but the form takes no negative amount.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = -1,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "ForeignTaxPaid", "A Bank");
}

TEST(DhoXmlWriterTest, PayerCountryInLowercaseIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mCountry = "de"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "PayerCountryCode", "A Bank");
}

TEST(DhoXmlWriterTest, SourceCountryOfThreeLettersIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "DEU",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "SourceCountryCode", "A Bank");
}

TEST(DhoXmlWriterTest, PayerTaxNumberOfSevenDigitsIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mCountry = "SI", .mTaxNumber = "1234567"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "SI",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "TaxID", "A Bank");
}

TEST(DhoXmlWriterTest, PayerNameWithAControlCharacterIsRejected) {
    // The name is not valid XML text, so it is not part of the error either.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A\x01 Bank", .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "Title", std::nullopt);
}

TEST(DhoXmlWriterTest, PayerAddressWithABrokenUtf8CharacterIsRejected) {
    // "\xC3\x28": a lead byte of a two-byte character followed by a plain "(".
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mAddress = "1 Main \xC3\x28", .mCountry = "DE"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "Address", "A Bank");
}

TEST(DhoXmlWriterTest, PayerIdentificationNumberWithAControlCharacterIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
            .mPayer = {.mName = "A Bank", .mCountry = "DE", .mIdentificationNumber = "DE30\x02"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 1'000'000,
            .mForeignTax = 0,
            .mSourceCountry = "DE",
        }}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "IDeu", "A Bank");
}

TEST(DhoXmlWriterTest, YearBefore2006IsRejected) {
    // The schema accepts a year from 2006.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2005,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{.mRows = {}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "Year", std::nullopt);
}

TEST(DhoXmlWriterTest, Year2006IsAccepted) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2006,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{.mRows = {}};

    const auto result = DhoXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "Year"), std::vector<std::string>{"2006"});
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, YearBelow1000GivesOneYearError) {
    // The shared check reports it; the 2006 check must not report it a second time.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 999,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{.mRows = {}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "Year", std::nullopt);
}

TEST(DhoXmlWriterTest, TaxNumberWithSevenDigitsIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "1234567", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{.mRows = {}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "taxNumber", std::nullopt);
}

TEST(DhoXmlWriterTest, ThePhoneNumberOfTheTaxpayerMustBeValidXmlText) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true, .mPhoneNumber = "1\x03"},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{.mRows = {}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "PhoneNumber", std::nullopt);
}

TEST(DhoXmlWriterTest, TheEmailOfTheTaxpayerMustBeValidXmlText) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true, .mEmail = "a\x03@b.si"},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{.mRows = {}};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "Email", std::nullopt);
}

TEST(DhoXmlWriterTest, 999RowsAreWritten) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DhoInterestRow row{
        .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
        .mPayer = {.mName = "A Bank", .mCountry = "DE"},
        .mKind = DepositInterestKind::BankDeposit,
        .mInterest = 10'000,
        .mForeignTax = 0,
        .mSourceCountry = "DE",
    };
    const DepositInterestReportDHO report{.mRows = std::vector<DhoInterestRow>(999, row)};

    const auto result = DhoXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "InterestEarned").size(), 999U);
    // 999 rows of EUR 1.00.
    EXPECT_EQ(textsOf(*result.mXml, "TotalInterestEarned"), std::vector<std::string>{"999.00"});
    expectValidForDhoSchema(*result.mXml);
}

TEST(DhoXmlWriterTest, 1000RowsAreRejectedWithOneError) {
    // The schema allows at most 999 interest rows.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DhoInterestRow row{
        .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
        .mPayer = {.mName = "A Bank", .mCountry = "DE"},
        .mKind = DepositInterestKind::BankDeposit,
        .mInterest = 10'000,
        .mForeignTax = 0,
        .mSourceCountry = "DE",
    };
    const DepositInterestReportDHO report{.mRows = std::vector<DhoInterestRow>(1000, row)};

    const auto result = DhoXmlWriter::write(metadata, report);

    expectOneError(result, "Doh_DHO_InterestEarned", std::nullopt);
}

TEST(DhoXmlWriterTest, SeveralProblemsGiveOneDiagnosticEachInRowOrder) {
    // Row 1: interest 0. Row 2: tax EUR -0.0001 (-1 at 4 decimals). Two errors, in row order, each
    // with its own payer name.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DepositInterestReportDHO report{
        .mRows = {
            {
                .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}}},
                .mPayer = {.mName = "A Bank", .mCountry = "DE"},
                .mKind = DepositInterestKind::BankDeposit,
                .mInterest = 0,
                .mForeignTax = 0,
                .mSourceCountry = "DE",
            },
            {
                .mPayments = {{StatementEventKind::Interest, std::size_t{1}, std::size_t{0}}},
                .mPayer = {.mName = "B Bank", .mCountry = "DE"},
                .mKind = DepositInterestKind::BankDeposit,
                .mInterest = 1'000'000,
                .mForeignTax = -1,
                .mSourceCountry = "DE",
            },
        }};

    const auto result = DhoXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mField, "InterestEarned");
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mInstrumentName, "A Bank");
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mField, "ForeignTaxPaid");
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mInstrumentName, "B Bank");
}

} // namespace
