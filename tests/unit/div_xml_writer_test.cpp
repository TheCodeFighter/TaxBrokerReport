#include "../support/xsd_validation.hpp"
#include "utils/date_utils.hpp"
#include "xml/div_xml_writer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;
using taxbroker::test::expectInvalidForDivSchema;
using taxbroker::test::expectValidForDivSchema;

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

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

TEST(DivXmlWriterTest, OneDividendWritesTheWholeDocument) {
    // Gross EUR 12.3450 is 12.35 (half cent up), tax EUR 1.8518 is 1.85.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{
        .mRows = {{
            .mPayment = {StatementEventKind::Dividend, 0, 0},
            .mIsin = "US0000000001",
            .mDate = day("2025-03-14"),
            .mPayer =
                {
                    .mName = "Example Corp",
                    .mAddress = "1 Main St, Springfield",
                    .mCountry = "US",
                    .mIdentificationNumber = "12-3456789",
                },
            .mKind = DividendKind::Ordinary,
            .mGrossIncome = 123'450,
            .mWithholding = {.mForeignTax = 18'518, .mBlankInExport = false},
            .mSourceCountry = "US",
            .mTreatyClaim = std::nullopt,
        }}};

    const auto result = DivXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // The schema has no edp:bodyContent in body, and the Dividend rows follow Doh_Div. The ISIN
    // is not written, and the payer has no Slovenian tax number.
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Div_3.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_Div>
      <Period>2025</Period>
      <IsResident>true</IsResident>
      <SelfReport>false</SelfReport>
    </Doh_Div>
    <Dividend>
      <Date>2025-03-14</Date>
      <PayerIdentificationNumber>12-3456789</PayerIdentificationNumber>
      <PayerName>Example Corp</PayerName>
      <PayerAddress>1 Main St, Springfield</PayerAddress>
      <PayerCountry>US</PayerCountry>
      <Type>1</Type>
      <Value>12.35</Value>
      <ForeignTax>1.85</ForeignTax>
      <SourceCountry>US</SourceCountry>
      <ReliefStatement>false</ReliefStatement>
    </Dividend>
  </body>
</Envelope>
)xml");
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, TaxpayerContactFieldsAreWrittenInSchemaOrder) {
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
    const DividendReportDiv report{.mRows = {}};

    const auto result = DivXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // Doh_Div order: Period, EmailAddress, PhoneNumber, IsResident, SelfReport.
    EXPECT_NE(result.mXml->find("<Doh_Div>\n      <Period>2025</Period>\n"
                                "      <EmailAddress>taxpayer@example.com</EmailAddress>\n"
                                "      <PhoneNumber>+38600000000</PhoneNumber>\n"
                                "      <IsResident>true</IsResident>\n"
                                "      <SelfReport>false</SelfReport>\n"
                                "    </Doh_Div>"),
              std::string::npos);
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, SelfReportWritesItsWorkflowCode) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::SelfReport,
    };
    const DividendReportDiv report{.mRows = {}};

    const auto result = DivXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "edp:DocumentWorkflowID"), std::vector<std::string>{"I"});
    EXPECT_EQ(textsOf(*result.mXml, "edp:DocumentWorkflowName"),
              std::vector<std::string>{"SamoPrijava"});
    EXPECT_EQ(textsOf(*result.mXml, "SelfReport"), std::vector<std::string>{"true"});
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, PayerNotFoundLeavesThePayerFieldsOut) {
    // No payer facts and no source country: only the facts of the payment are written.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {{
                                       .mPayment = {StatementEventKind::Dividend, 0, 0},
                                       .mIsin = "US0000000001",
                                       .mDate = day("2025-03-14"),
                                       .mPayer = {},
                                       .mKind = DividendKind::Ordinary,
                                       .mGrossIncome = 100'000,
                                       .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                                       .mSourceCountry = std::nullopt,
                                       .mTreatyClaim = std::nullopt,
                                   }}};

    const auto result = DivXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_NE(result.mXml->find("    <Dividend>\n"
                                "      <Date>2025-03-14</Date>\n"
                                "      <Type>1</Type>\n"
                                "      <Value>10.00</Value>\n"
                                "      <ForeignTax>0.00</ForeignTax>\n"
                                "      <ReliefStatement>false</ReliefStatement>\n"
                                "    </Dividend>"),
              std::string::npos);
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, SlovenianTaxNumberComesBeforeTheIdentificationNumber) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{
        .mRows = {{
            .mPayment = {StatementEventKind::Dividend, 0, 0},
            .mIsin = "SI0000000001",
            .mDate = day("2025-03-14"),
            .mPayer = {.mTaxNumber = "87654321", .mIdentificationNumber = "ID-1"},
            .mKind = DividendKind::Ordinary,
            .mGrossIncome = 100'000,
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
            .mSourceCountry = std::nullopt,
            .mTreatyClaim = std::nullopt,
        }}};

    const auto result = DivXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_NE(
        result.mXml->find("      <Date>2025-03-14</Date>\n"
                          "      <PayerTaxNumber>87654321</PayerTaxNumber>\n"
                          "      <PayerIdentificationNumber>ID-1</PayerIdentificationNumber>\n"
                          "      <Type>1</Type>"),
        std::string::npos);
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, TheIsinIsNeverWritten) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {{
                                       .mPayment = {StatementEventKind::Dividend, 0, 0},
                                       .mIsin = "US0000000001",
                                       .mDate = day("2025-03-14"),
                                       .mPayer = {.mName = "Example Corp", .mCountry = "US"},
                                       .mKind = DividendKind::Ordinary,
                                       .mGrossIncome = 100'000,
                                       .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                                       .mSourceCountry = "US",
                                       .mTreatyClaim = std::nullopt,
                                   }}};

    const auto result = DivXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(result.mXml->find("US0000000001"), std::string::npos);
    EXPECT_TRUE(textsOf(*result.mXml, "PayerIdentificationNumber").empty());
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, HalfCentsRoundAwayFromZeroOnce) {
    // Money has four decimals, the form two. Gross 12.3450 -> 12.35, 12.3449 -> 12.34,
    // 0.0050 -> 0.01. Tax 0.0049 -> 0.00, 0.0150 -> 0.02, 0 -> 0.00.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Dividend, 0, 0},
                .mIsin = "US0000000001",
                .mDate = day("2025-03-14"),
                .mPayer = {},
                .mKind = DividendKind::Ordinary,
                .mGrossIncome = 123'450,
                .mWithholding = {.mForeignTax = 49, .mBlankInExport = false},
                .mSourceCountry = std::nullopt,
                .mTreatyClaim = std::nullopt,
            },
            {
                .mPayment = {StatementEventKind::Dividend, 0, 1},
                .mIsin = "US0000000001",
                .mDate = day("2025-03-15"),
                .mPayer = {},
                .mKind = DividendKind::Ordinary,
                .mGrossIncome = 123'449,
                .mWithholding = {.mForeignTax = 150, .mBlankInExport = false},
                .mSourceCountry = std::nullopt,
                .mTreatyClaim = std::nullopt,
            },
            {
                .mPayment = {StatementEventKind::Dividend, 0, 2},
                .mIsin = "US0000000001",
                .mDate = day("2025-03-16"),
                .mPayer = {},
                .mKind = DividendKind::Ordinary,
                .mGrossIncome = 50,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                .mSourceCountry = std::nullopt,
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = DivXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "Value"), (std::vector<std::string>{"12.35", "12.34", "0.01"}));
    EXPECT_EQ(textsOf(*result.mXml, "ForeignTax"),
              (std::vector<std::string>{"0.00", "0.02", "0.00"}));
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, WholeAmountsKeepTwoDecimals) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{
        .mRows = {{
            .mPayment = {StatementEventKind::Dividend, 0, 0},
            .mIsin = "US0000000001",
            .mDate = day("2025-03-14"),
            .mPayer = {},
            .mKind = DividendKind::Ordinary,
            .mGrossIncome = 100'000,                                          // EUR 10
            .mWithholding = {.mForeignTax = 25'000, .mBlankInExport = false}, // EUR 2.5
            .mSourceCountry = std::nullopt,
            .mTreatyClaim = std::nullopt,
        }}};

    const auto result = DivXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "Value"), std::vector<std::string>{"10.00"});
    EXPECT_EQ(textsOf(*result.mXml, "ForeignTax"), std::vector<std::string>{"2.50"});
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, LargestMoneyValueFitsWithoutOverflow) {
    // 922337203685477.5807 is 9223372036854775807 stored steps: 92233720368547758 cents and a
    // remainder of 7 steps, so it rounds down to 922337203685477.58.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{
        .mRows = {{
            .mPayment = {StatementEventKind::Dividend, 0, 0},
            .mIsin = "US0000000001",
            .mDate = day("2025-03-14"),
            .mPayer = {},
            .mKind = DividendKind::Ordinary,
            .mGrossIncome = 9'223'372'036'854'775'807,
            .mWithholding = {.mForeignTax = 9'223'372'036'854'775'807, .mBlankInExport = false},
            .mSourceCountry = std::nullopt,
            .mTreatyClaim = std::nullopt,
        }}};

    const auto result = DivXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "Value"), std::vector<std::string>{"922337203685477.58"});
    EXPECT_EQ(textsOf(*result.mXml, "ForeignTax"), std::vector<std::string>{"922337203685477.58"});
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, RowsKeepTheModelOrder) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Dividend, 1, 0},
                .mIsin = "US0000000002",
                .mDate = day("2025-01-10"),
                .mPayer = {.mName = "Beta Inc"},
                .mKind = DividendKind::Ordinary,
                .mGrossIncome = 20'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                .mSourceCountry = std::nullopt,
                .mTreatyClaim = std::nullopt,
            },
            {
                .mPayment = {StatementEventKind::Dividend, 0, 0},
                .mIsin = "US0000000001",
                .mDate = day("2025-01-10"),
                .mPayer = {.mName = "Alpha Inc"},
                .mKind = DividendKind::Ordinary,
                .mGrossIncome = 10'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                .mSourceCountry = std::nullopt,
                .mTreatyClaim = std::nullopt,
            },
            {
                .mPayment = {StatementEventKind::Dividend, 0, 1},
                .mIsin = "US0000000001",
                .mDate = day("2025-06-30"),
                .mPayer = {.mName = "Alpha Inc"},
                .mKind = DividendKind::Ordinary,
                .mGrossIncome = 30'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                .mSourceCountry = std::nullopt,
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = DivXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "PayerName"),
              (std::vector<std::string>{"Beta Inc", "Alpha Inc", "Alpha Inc"}));
    EXPECT_EQ(textsOf(*result.mXml, "Value"), (std::vector<std::string>{"2.00", "1.00", "3.00"}));
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, ATreatyClaimWritesTrueWithoutTheTreatyText) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{
        .mRows = {{
            .mPayment = {StatementEventKind::Dividend, 0, 0},
            .mIsin = "US0000000001",
            .mDate = day("2025-03-14"),
            .mPayer = {.mCountry = "US"},
            .mKind = DividendKind::Ordinary,
            .mGrossIncome = 100'000,
            .mWithholding = {.mForeignTax = 15'000, .mBlankInExport = false},
            .mSourceCountry = "US",
            .mTreatyClaim =
                TreatyClaim{.mStatement = "Article 10(2)", .mCatalogueRow = std::nullopt},
        }}};

    const auto result = DivXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "ReliefStatement"), std::vector<std::string>{"true"});
    EXPECT_EQ(result.mXml->find("Article 10(2)"), std::string::npos);
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, TextIsEscapedAndNonAsciiIsKept) {
    // &, < and > are escaped; quotes need no escaping in element text; letters stay as they are.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{
        .mRows = {{
            .mPayment = {StatementEventKind::Dividend, 0, 0},
            .mIsin = "DE0000000001",
            .mDate = day("2025-03-14"),
            .mPayer = {.mName = "Müller & Söhne <AG>", .mAddress = R"(Straße "1")"},
            .mKind = DividendKind::Ordinary,
            .mGrossIncome = 100'000,
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
            .mSourceCountry = std::nullopt,
            .mTreatyClaim = std::nullopt,
        }}};

    const auto result = DivXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "PayerName"),
              std::vector<std::string>{"Müller &amp; Söhne &lt;AG&gt;"});
    EXPECT_EQ(textsOf(*result.mXml, "PayerAddress"), std::vector<std::string>{R"(Straße "1")"});
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, AnEmptyReportWritesNoDividend) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {}};

    const auto result = DivXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(result.mXml->find("<Dividend>"), std::string::npos);
    EXPECT_NE(result.mXml->find("<Doh_Div>"), std::string::npos);
    expectValidForDivSchema(*result.mXml);
}

TEST(DivXmlWriterTest, SchemaRejectsAValueWithThreeDecimals) {
    // Proves that the schema check can fail: Value has two decimals at most.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Div_3.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_Div>
      <Period>2025</Period>
    </Doh_Div>
    <Dividend>
      <Date>2025-03-14</Date>
      <Value>1.234</Value>
    </Dividend>
  </body>
</Envelope>
)xml";

    expectInvalidForDivSchema(xml);
}

/// One error, no XML: the field, the report and the row's ISIN (empty for a header problem).
void expectOneError(const XmlWriteResult& aResult,
                    std::string_view aField,
                    std::optional<std::string_view> aIsin) {
    EXPECT_FALSE(aResult.mXml.has_value());
    ASSERT_EQ(aResult.mDiagnostics.size(), 1U);

    const auto& diagnostic = aResult.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports, std::vector{ReportType::Dividends});
    EXPECT_EQ(diagnostic.mContext.mField, aField);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", std::string{aField})});

    if (aIsin)
    {
        EXPECT_EQ(diagnostic.mContext.mIsin, std::string{*aIsin});
    }
    else
    {
        EXPECT_FALSE(diagnostic.mContext.mIsin.has_value());
    }
}

TEST(DivXmlWriterTest, GrossOfZeroIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {{
                                       .mPayment = {StatementEventKind::Dividend, 0, 0},
                                       .mIsin = "US0000000001",
                                       .mDate = day("2025-03-14"),
                                       .mPayer = {},
                                       .mKind = DividendKind::Ordinary,
                                       .mGrossIncome = 0,
                                       .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                                       .mSourceCountry = std::nullopt,
                                       .mTreatyClaim = std::nullopt,
                                   }}};

    const auto result = DivXmlWriter::write(metadata, report);

    expectOneError(result, "Value", "US0000000001");
}

TEST(DivXmlWriterTest, GrossThatRoundsToZeroIsRejected) {
    // EUR 0.0049 rounds to 0.00.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {{
                                       .mPayment = {StatementEventKind::Dividend, 0, 0},
                                       .mIsin = "US0000000001",
                                       .mDate = day("2025-03-14"),
                                       .mPayer = {},
                                       .mKind = DividendKind::Ordinary,
                                       .mGrossIncome = 49,
                                       .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                                       .mSourceCountry = std::nullopt,
                                       .mTreatyClaim = std::nullopt,
                                   }}};

    const auto result = DivXmlWriter::write(metadata, report);

    expectOneError(result, "Value", "US0000000001");
}

TEST(DivXmlWriterTest, NegativeGrossIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {{
                                       .mPayment = {StatementEventKind::Dividend, 0, 0},
                                       .mIsin = "US0000000001",
                                       .mDate = day("2025-03-14"),
                                       .mPayer = {},
                                       .mKind = DividendKind::Ordinary,
                                       .mGrossIncome = -100'000, // EUR -10
                                       .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                                       .mSourceCountry = std::nullopt,
                                       .mTreatyClaim = std::nullopt,
                                   }}};

    const auto result = DivXmlWriter::write(metadata, report);

    expectOneError(result, "Value", "US0000000001");
}

TEST(DivXmlWriterTest, NegativeForeignTaxIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {{
                                       .mPayment = {StatementEventKind::Dividend, 0, 0},
                                       .mIsin = "US0000000001",
                                       .mDate = day("2025-03-14"),
                                       .mPayer = {},
                                       .mKind = DividendKind::Ordinary,
                                       .mGrossIncome = 100'000,
                                       .mWithholding = {.mForeignTax = -1, .mBlankInExport = false},
                                       .mSourceCountry = std::nullopt,
                                       .mTreatyClaim = std::nullopt,
                                   }}};

    const auto result = DivXmlWriter::write(metadata, report);

    expectOneError(result, "ForeignTax", "US0000000001");
}

TEST(DivXmlWriterTest, PayerCountryInLowercaseIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {{
                                       .mPayment = {StatementEventKind::Dividend, 0, 0},
                                       .mIsin = "US0000000001",
                                       .mDate = day("2025-03-14"),
                                       .mPayer = {.mCountry = "us"},
                                       .mKind = DividendKind::Ordinary,
                                       .mGrossIncome = 100'000,
                                       .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                                       .mSourceCountry = std::nullopt,
                                       .mTreatyClaim = std::nullopt,
                                   }}};

    const auto result = DivXmlWriter::write(metadata, report);

    expectOneError(result, "PayerCountry", "US0000000001");
}

TEST(DivXmlWriterTest, SourceCountryOfThreeLettersIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {{
                                       .mPayment = {StatementEventKind::Dividend, 0, 0},
                                       .mIsin = "US0000000001",
                                       .mDate = day("2025-03-14"),
                                       .mPayer = {},
                                       .mKind = DividendKind::Ordinary,
                                       .mGrossIncome = 100'000,
                                       .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                                       .mSourceCountry = "USA",
                                       .mTreatyClaim = std::nullopt,
                                   }}};

    const auto result = DivXmlWriter::write(metadata, report);

    expectOneError(result, "SourceCountry", "US0000000001");
}

TEST(DivXmlWriterTest, PayerNameWithAControlCharacterIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{
        .mRows = {{
            .mPayment = {StatementEventKind::Dividend, 0, 0},
            .mIsin = "US0000000001",
            .mDate = day("2025-03-14"),
            .mPayer = {.mName = std::string{"Bad\x01"} + "Name"}, // U+0001 is not XML text
            .mKind = DividendKind::Ordinary,
            .mGrossIncome = 100'000,
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
            .mSourceCountry = std::nullopt,
            .mTreatyClaim = std::nullopt,
        }}};

    const auto result = DivXmlWriter::write(metadata, report);

    expectOneError(result, "PayerName", "US0000000001");
    // The broken name is not copied into the context.
    EXPECT_FALSE(result.mDiagnostics.front().mContext.mInstrumentName.has_value());
}

TEST(DivXmlWriterTest, PayerAddressWithABrokenUtf8CharacterIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{
        .mRows = {{
            .mPayment = {StatementEventKind::Dividend, 0, 0},
            .mIsin = "US0000000001",
            .mDate = day("2025-03-14"),
            // 0xC3 starts a two-byte character, '(' is not a continuation byte.
            .mPayer = {.mName = "Example Corp", .mAddress = "Stra\xC3("},
            .mKind = DividendKind::Ordinary,
            .mGrossIncome = 100'000,
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
            .mSourceCountry = std::nullopt,
            .mTreatyClaim = std::nullopt,
        }}};

    const auto result = DivXmlWriter::write(metadata, report);

    expectOneError(result, "PayerAddress", "US0000000001");
    EXPECT_EQ(result.mDiagnostics.front().mContext.mInstrumentName, "Example Corp");
}

TEST(DivXmlWriterTest, PayerTaxNumberOfSevenDigitsIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {{
                                       .mPayment = {StatementEventKind::Dividend, 0, 0},
                                       .mIsin = "SI0000000001",
                                       .mDate = day("2025-03-14"),
                                       .mPayer = {.mTaxNumber = "1234567"},
                                       .mKind = DividendKind::Ordinary,
                                       .mGrossIncome = 100'000,
                                       .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                                       .mSourceCountry = std::nullopt,
                                       .mTreatyClaim = std::nullopt,
                                   }}};

    const auto result = DivXmlWriter::write(metadata, report);

    expectOneError(result, "PayerTaxNumber", "SI0000000001");
}

TEST(DivXmlWriterTest, DateOutsideTheReportingYearIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {{
                                       .mPayment = {StatementEventKind::Dividend, 0, 0},
                                       .mIsin = "US0000000001",
                                       .mDate = day("2024-12-31"),
                                       .mPayer = {},
                                       .mKind = DividendKind::Ordinary,
                                       .mGrossIncome = 100'000,
                                       .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                                       .mSourceCountry = std::nullopt,
                                       .mTreatyClaim = std::nullopt,
                                   }}};

    const auto result = DivXmlWriter::write(metadata, report);

    expectOneError(result, "Date", "US0000000001");
}

TEST(DivXmlWriterTest, TaxNumberWithSevenDigitsIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "1234567", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {}};

    const auto result = DivXmlWriter::write(metadata, report);

    expectOneError(result, "taxNumber", std::nullopt);
}

TEST(DivXmlWriterTest, YearBelow1000IsRejectedAsPeriod) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 999,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{.mRows = {}};

    const auto result = DivXmlWriter::write(metadata, report);

    expectOneError(result, "Period", std::nullopt);
}

TEST(DivXmlWriterTest, SeveralProblemsGiveOneDiagnosticEachInRowOrder) {
    // Row 1: gross 0. Row 2: tax -1. Two errors, in row order, each with its own ISIN.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const DividendReportDiv report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Dividend, 0, 0},
                .mIsin = "US0000000001",
                .mDate = day("2025-03-14"),
                .mPayer = {},
                .mKind = DividendKind::Ordinary,
                .mGrossIncome = 0,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
                .mSourceCountry = std::nullopt,
                .mTreatyClaim = std::nullopt,
            },
            {
                .mPayment = {StatementEventKind::Dividend, 1, 0},
                .mIsin = "US0000000002",
                .mDate = day("2025-03-15"),
                .mPayer = {},
                .mKind = DividendKind::Ordinary,
                .mGrossIncome = 100'000,
                .mWithholding = {.mForeignTax = -1, .mBlankInExport = false},
                .mSourceCountry = std::nullopt,
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = DivXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mField, "Value");
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mIsin, "US0000000001");
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mField, "ForeignTax");
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mIsin, "US0000000002");
}

} // namespace
