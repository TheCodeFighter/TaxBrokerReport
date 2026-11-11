#include "../support/xsd_validation.hpp"
#include "utils/date_utils.hpp"
#include "xml/obr_xml_writer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;
using taxbroker::test::expectInvalidForObrSchema;
using taxbroker::test::expectValidForObrSchema;

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

/// One error, no XML: the field, the report, the payer name and the ISIN of a coupon (each empty
/// when the row has none or it is not valid XML text).
void expectOneError(const XmlWriteResult& aResult,
                    std::string_view aField,
                    std::optional<std::string_view> aName,
                    std::optional<std::string_view> aIsin) {
    EXPECT_FALSE(aResult.mXml.has_value());
    ASSERT_EQ(aResult.mDiagnostics.size(), 1U);

    const auto& diagnostic = aResult.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports, std::vector{ReportType::OtherInterest});
    EXPECT_EQ(diagnostic.mContext.mField, aField);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", std::string{aField})});

    if (aName)
    {
        EXPECT_EQ(diagnostic.mContext.mInstrumentName, std::string{*aName});
    }
    else
    {
        EXPECT_FALSE(diagnostic.mContext.mInstrumentName.has_value());
    }

    if (aIsin)
    {
        EXPECT_EQ(diagnostic.mContext.mIsin, std::string{*aIsin});
    }
    else
    {
        EXPECT_FALSE(diagnostic.mContext.mIsin.has_value());
    }
}

TEST(ObrXmlWriterTest, BothSubtypesWriteTheWholeDocument) {
    // Cash interest EUR 12.34 without tax, and a coupon of EUR 25.00 with EUR 3.75 foreign tax.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
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

    const auto result = ObrXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_Obr>
      <SelfReport>false</SelfReport>
      <Period>2025</Period>
      <DocumentWorkflowID>O</DocumentWorkflowID>
      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>
      <Country>SI</Country>
      <Interest>
        <Date>2025-03-31</Date>
        <IdentificationNumber>DE000000000</IdentificationNumber>
        <Name>Example Broker AG</Name>
        <Address>Example Street 1, 10115 Berlin</Address>
        <Country>DE</Country>
        <Type>4</Type>
        <Value>12.34</Value>
        <ForeignTax>0.00</ForeignTax>
        <Country2>DE</Country2>
      </Interest>
      <Interest>
        <Date>2025-06-15</Date>
        <IdentificationNumber>FR0000000000</IdentificationNumber>
        <Name>Example Issuer SA</Name>
        <Address>1 Rue Exemple, 75001 Paris</Address>
        <Country>FR</Country>
        <Type>2</Type>
        <Value>25.00</Value>
        <ForeignTax>3.75</ForeignTax>
        <Country2>FR</Country2>
      </Interest>
    </Doh_Obr>
  </body>
</Envelope>
)xml");
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, TaxpayerContactFieldsAreWrittenInSchemaOrder) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678",
                      .mIsResident = true,
                      .mEmail = "taxpayer@example.com",
                      .mPhoneNumber = "+38600000000"},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{.mRows = {}};

    const auto result = ObrXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // Doh_Obr order: SelfReport, Period, DocumentWorkflowID, Email, TelephoneNumber, resident,
    // Country.
    EXPECT_NE(result.mXml->find(
                  "      <DocumentWorkflowID>O</DocumentWorkflowID>\n"
                  "      <Email>taxpayer@example.com</Email>\n"
                  "      <TelephoneNumber>+38600000000</TelephoneNumber>\n"
                  "      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>\n"),
              std::string::npos);
    expectValidForObrSchema(*result.mXml);
}

// eDavki rejects the self-report workflow for this form, so a self-report run writes an
// original.
TEST(ObrXmlWriterTest, SelfReportRunIsStillWrittenAsAnOriginal) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::SelfReport,
    };
    const OtherInterestReportObr report{.mRows = {}};

    const auto result = ObrXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "edp:DocumentWorkflowID"), std::vector<std::string>{"O"});
    EXPECT_EQ(textsOf(*result.mXml, "edp:DocumentWorkflowName"),
              std::vector<std::string>{"Original"});
    EXPECT_EQ(textsOf(*result.mXml, "DocumentWorkflowID"), std::vector<std::string>{"O"});
    EXPECT_EQ(textsOf(*result.mXml, "SelfReport"), std::vector<std::string>{"false"});
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, ANonResidentWritesNoCountryOfResidence) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = false},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{.mRows = {}};

    const auto result = ObrXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "ResidentOfRepublicOfSlovenia"),
              std::vector<std::string>{"false"});
    EXPECT_EQ(result.mXml->find("<Country>"), std::string::npos);
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, PayerNotFoundLeavesThePayerFieldsOut) {
    // No payer facts and no source country: only the facts of the payment are written.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {},
                .mGrossInterest = 123'400,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = std::nullopt,
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_NE(result.mXml->find("      <Interest>\n"
                                "        <Date>2025-03-31</Date>\n"
                                "        <Type>4</Type>\n"
                                "        <Value>12.34</Value>\n"
                                "        <ForeignTax>0.00</ForeignTax>\n"
                                "      </Interest>"),
              std::string::npos);
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, SlovenianIssuerWritesTaxNumberAndNoNameAddressOrForeignTax) {
    // A Slovenian issuer with a tax number: `TaxNumber`, no `IdentificationNumber`, no `Name` and
    // `Address` (eDavki fills them in from the tax register) and no foreign tax.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "SI0000000001"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "Example Izdajatelj d.d.",
                           .mAddress = "1 Main St, Ljubljana",
                           .mCountry = "SI",
                           .mTaxNumber = "12345679"},
                .mGrossInterest = 100'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "SI",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_NE(result.mXml->find("      <Interest>\n"
                                "        <Date>2025-06-15</Date>\n"
                                "        <TaxNumber>12345679</TaxNumber>\n"
                                "        <Country>SI</Country>\n"
                                "        <Type>2</Type>\n"
                                "        <Value>10.00</Value>\n"
                                "        <Country2>SI</Country2>\n"
                                "      </Interest>"),
              std::string::npos);
    EXPECT_TRUE(textsOf(*result.mXml, "ForeignTax").empty());
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, SlovenianIssuerWithoutATaxNumberKeepsNameAndAddress) {
    // No valid tax number is known (the stage warned): the name and address are written, so the
    // user sees whom the row is about and completes the number in eDavki.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "SI0000000001"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "Example Izdajatelj d.d.",
                           .mAddress = "1 Main St, Ljubljana",
                           .mCountry = "SI"},
                .mGrossInterest = 100'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "SI",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_NE(result.mXml->find("      <Interest>\n"
                                "        <Date>2025-06-15</Date>\n"
                                "        <Name>Example Izdajatelj d.d.</Name>\n"
                                "        <Address>1 Main St, Ljubljana</Address>\n"
                                "        <Country>SI</Country>\n"
                                "        <Type>2</Type>\n"
                                "        <Value>10.00</Value>\n"
                                "        <Country2>SI</Country2>\n"
                                "      </Interest>"),
              std::string::npos);
    EXPECT_TRUE(textsOf(*result.mXml, "TaxNumber").empty());
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, SlovenianIssuerWithATaxIsWrittenWithoutForeignTax) {
    // Tax EUR 5.0000 of a Slovenian issuer is a Slovenian withholding, not foreign tax: the row is
    // written without it and without an error (the stage already warned).
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "SI0000000001"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "Example Izdajatelj d.d.",
                           .mAddress = "1 Main St, Ljubljana",
                           .mCountry = "SI",
                           .mTaxNumber = "12345679"},
                .mGrossInterest = 100'000,
                .mWithholding = {.mForeignTax = 50'000, .mBlankInExport = false},
                .mSourceCountry = "SI",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_TRUE(textsOf(*result.mXml, "ForeignTax").empty());
    EXPECT_EQ(textsOf(*result.mXml, "Value"), std::vector<std::string>{"10.00"});
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, HalfCentsRoundAwayFromZeroOnce) {
    // Row 1: gross 10.0049 is 10.00, tax 0.0050 is 0.01.
    // Row 2: gross 10.0050 is 10.01, tax 0.0049 is 0.00.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 100'049,
                .mWithholding = {.mForeignTax = 50, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{1}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-04-30"),
                .mPayer = {.mName = "B Broker", .mCountry = "DE"},
                .mGrossInterest = 100'050,
                .mWithholding = {.mForeignTax = 49, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "Value"), (std::vector<std::string>{"10.00", "10.01"}));
    EXPECT_EQ(textsOf(*result.mXml, "ForeignTax"), (std::vector<std::string>{"0.01", "0.00"}));
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, RowsKeepTheModelOrder) {
    // The coupon of June comes before the cash interest of March: the model order is kept.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "FR0000000001"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "Coupon Issuer", .mCountry = "FR"},
                .mGrossInterest = 250'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "FR",
                .mTreatyClaim = std::nullopt,
            },
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{1}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "Cash Broker", .mCountry = "DE"},
                .mGrossInterest = 123'400,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "Name"),
              (std::vector<std::string>{"Coupon Issuer", "Cash Broker"}));
    EXPECT_EQ(textsOf(*result.mXml, "Type"), (std::vector<std::string>{"2", "4"}));
    EXPECT_EQ(textsOf(*result.mXml, "Date"),
              (std::vector<std::string>{"2025-06-15", "2025-03-31"}));
}

TEST(ObrXmlWriterTest, TextIsEscapedAndNonAsciiIsKept) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "FR0000000001"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "A & B <Issuer>",
                           .mAddress = "Šparkasse, Dunaj",
                           .mCountry = "AT"},
                .mGrossInterest = 250'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "AT",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_NE(result.mXml->find("<Name>A &amp; B &lt;Issuer&gt;</Name>"), std::string::npos);
    EXPECT_NE(result.mXml->find("<Address>Šparkasse, Dunaj</Address>"), std::string::npos);
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, AnEmptyReportWritesNoInterestRow) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{.mRows = {}};

    const auto result = ObrXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(result.mXml->find("<Interest>"), std::string::npos);
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, LargestValueThatFitsTheSchemaIsWritten) {
    // EUR 999999999999.9949 is 999999999999.99, the largest value of 12 digits and 2 decimals.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 9'999'999'999'999'949,
                .mWithholding = {.mForeignTax = 9'999'999'999'999'949, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "Value"), std::vector<std::string>{"999999999999.99"});
    EXPECT_EQ(textsOf(*result.mXml, "ForeignTax"), std::vector<std::string>{"999999999999.99"});
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, AValueThatRoundsAboveTheSchemaLimitIsRejected) {
    // EUR 999999999999.9950 rounds up to 1000000000000.00, which has 13 digits.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 9'999'999'999'999'950,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Value", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, AForeignTaxAboveTheSchemaLimitIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 9'999'999'999'999'950, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "ForeignTax", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, GrossOfZeroIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 0,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Value", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, GrossThatRoundsToZeroIsRejected) {
    // EUR 0.0049 is 0.00.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 49,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Value", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, NegativeGrossIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = -10'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Value", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, NegativeForeignTaxIsRejected) {
    // Tax EUR -0.0001 would round to 0.00, but the form takes no negative amount.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = -1, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "ForeignTax", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, DateOutsideTheReportingYearIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "FR0000000001"},
                .mDate = day("2024-12-31"),
                .mPayer = {.mName = "A Issuer", .mCountry = "FR"},
                .mGrossInterest = 250'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "FR",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Date", "A Issuer", "FR0000000001");
}

TEST(ObrXmlWriterTest, IdentificationNumberOf30CharactersIsAccepted) {
    // 30 characters is the limit of the schema.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker",
                           .mCountry = "DE",
                           .mIdentificationNumber = std::string(30, 'A')},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest,
     IdentificationNumberOf30TwoByteCharactersIsAcceptedBecauseCharactersAreCounted) {
    // 30 times "Š" is 30 characters and 60 bytes.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker",
                           .mCountry = "DE",
                           .mIdentificationNumber = std::string{"ŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠŠ"}},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, IdentificationNumberOf31CharactersIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker",
                           .mCountry = "DE",
                           .mIdentificationNumber = std::string(31, 'A')},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "IdentificationNumber", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, EmptyNameIsRejected) {
    // The schema type has no limit, but the instructions require a name; a missing name is left out
    // instead.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "", .mCountry = "DE"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Name", std::nullopt, std::nullopt);
}

TEST(ObrXmlWriterTest, EmptyAddressIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mAddress = "", .mCountry = "DE"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Address", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, PayerCountryInLowercaseIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "de"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Country", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, SourceCountryOfThreeLettersIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "FR"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "FRA",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Country2", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, PayerTaxNumberOfSevenDigitsIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "SI0000000001"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "A Issuer", .mCountry = "SI", .mTaxNumber = "1234567"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "SI",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "TaxNumber", "A Issuer", "SI0000000001");
}

TEST(ObrXmlWriterTest, PayerNameWithAControlCharacterIsRejected) {
    // The name is not valid XML text, so it is not part of the error either.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A\x01 Broker", .mCountry = "DE"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Name", std::nullopt, std::nullopt);
}

TEST(ObrXmlWriterTest, PayerAddressWithABrokenUtf8CharacterIsRejected) {
    // "\xC3\x28": a lead byte of a two-byte character followed by a plain "(".
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mAddress = "1 Main \xC3\x28", .mCountry = "DE"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Address", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, IsinThatIsNotValidXmlTextIsLeftOutOfTheError) {
    // An ISIN with a control character is not part of the error (the source country is the
    // problem).
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "FR00\x01Z"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "A Issuer", .mCountry = "FR"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "FRA",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Country2", "A Issuer", std::nullopt);
}

TEST(ObrXmlWriterTest, AClaimWithoutATextWritesDAOnTheClaimedRowOnly) {
    // The user claims the treaty and fills in the paragraph and article in eDavki, so the claimed
    // row has `DA`. The other row has no claim and no element.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "US0000000002"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "Example Issuer Inc",
                           .mAddress = "1 Example Street, New York",
                           .mCountry = "US",
                           .mIdentificationNumber = "US0000000000"},
                .mGrossInterest = 250'000,                                        // EUR 25.00
                .mWithholding = {.mForeignTax = 37'500, .mBlankInExport = false}, // EUR 3.75
                .mSourceCountry = "US",
                .mTreatyClaim =
                    TreatyClaim{.mStatement = std::nullopt, .mCatalogueRow = std::nullopt},
            },
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{1}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "FR0000000001"},
                .mDate = day("2025-07-15"),
                .mPayer = {.mName = "Example Issuer SA",
                           .mAddress = "1 Rue Exemple, 75001 Paris",
                           .mCountry = "FR",
                           .mIdentificationNumber = "FR0000000000"},
                .mGrossInterest = 100'000,                                        // EUR 10.00
                .mWithholding = {.mForeignTax = 15'000, .mBlankInExport = false}, // EUR 1.50
                .mSourceCountry = "FR",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_Obr>
      <SelfReport>false</SelfReport>
      <Period>2025</Period>
      <DocumentWorkflowID>O</DocumentWorkflowID>
      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>
      <Country>SI</Country>
      <Interest>
        <Date>2025-06-15</Date>
        <IdentificationNumber>US0000000000</IdentificationNumber>
        <Name>Example Issuer Inc</Name>
        <Address>1 Example Street, New York</Address>
        <Country>US</Country>
        <Type>2</Type>
        <Value>25.00</Value>
        <ForeignTax>3.75</ForeignTax>
        <Country2>US</Country2>
        <ReliefStatement>DA</ReliefStatement>
      </Interest>
      <Interest>
        <Date>2025-07-15</Date>
        <IdentificationNumber>FR0000000000</IdentificationNumber>
        <Name>Example Issuer SA</Name>
        <Address>1 Rue Exemple, 75001 Paris</Address>
        <Country>FR</Country>
        <Type>2</Type>
        <Value>10.00</Value>
        <ForeignTax>1.50</ForeignTax>
        <Country2>FR</Country2>
      </Interest>
    </Doh_Obr>
  </body>
</Envelope>
)xml");
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, AClaimWithATextWritesTheText) {
    // Nothing makes a text today; the model and the writer are ready for it.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 10'000, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = TreatyClaim{.mStatement = std::string{"2. odstavek 11. člena"},
                                            .mCatalogueRow = std::nullopt},
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "ReliefStatement"),
              std::vector<std::string>{"2. odstavek 11. člena"});
    expectValidForObrSchema(*result.mXml);
}

TEST(ObrXmlWriterTest, AClaimTextOf101CharactersStopsTheFile) {
    // The schema allows 100 characters; 101 is never cut or dropped.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 10'000, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim =
                    TreatyClaim{.mStatement = std::string(101, 'a'), .mCatalogueRow = std::nullopt},
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "ReliefStatement", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, AClaimTextWithASpaceAtItsStartStopsTheFile) {
    // A claim text is kept trimmed by the session; the writer never trims it.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 10'000, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = TreatyClaim{.mStatement = std::string{" 2. odstavek"},
                                            .mCatalogueRow = std::nullopt},
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "ReliefStatement", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, AClaimOnARowWithoutForeignTaxStopsTheFile) {
    // A treaty claim needs a foreign tax; it is never dropped silently.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim =
                    TreatyClaim{.mStatement = std::nullopt, .mCatalogueRow = std::nullopt},
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "ReliefStatement", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, AClaimOnASlovenianPayerStopsTheFile) {
    // The foreign tax of a Slovenian payer is not written, so a treaty claim has no tax to relieve.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "SI0000000001"},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "Issuer SI",
                           .mAddress = "1 Main Street, Ljubljana",
                           .mCountry = "SI",
                           .mTaxNumber = "12345679"},
                .mGrossInterest = 250'000,
                .mWithholding = {.mForeignTax = 62'500, .mBlankInExport = false},
                .mSourceCountry = "SI",
                .mTreatyClaim =
                    TreatyClaim{.mStatement = std::nullopt, .mCatalogueRow = std::nullopt},
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "ReliefStatement", "Issuer SI", "SI0000000001");
}

TEST(ObrXmlWriterTest, ASlovenianPayerWithAnIdentificationNumberStopsTheFile) {
    // A Slovenian payer has a tax number and no identification number (eDavki rejects it).
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "SI0000000001"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "Issuer SI",
                           .mAddress = "1 Main Street, Ljubljana",
                           .mCountry = "SI",
                           .mTaxNumber = "12345679",
                           .mIdentificationNumber = "12345679"},
                .mGrossInterest = 100'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "SI",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "IdentificationNumber", "Issuer SI", "SI0000000001");
}

TEST(ObrXmlWriterTest, ACouponErrorHasTheIsinAndTheName) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "FR0000000001"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "A Issuer", .mCountry = "FR"},
                .mGrossInterest = 0,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "FR",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Value", "A Issuer", "FR0000000001");
}

TEST(ObrXmlWriterTest, ACashInterestErrorHasTheNameOnly) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 0,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Value", "A Broker", std::nullopt);
}

TEST(ObrXmlWriterTest, SeveralProblemsGiveOneDiagnosticEachInRowOrder) {
    // Row 1: gross 0. Row 2: tax -1. Two errors, in row order, each with its own name.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "A Broker", .mCountry = "DE"},
                .mGrossInterest = 0,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{1}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "FR0000000002"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "B Issuer", .mCountry = "FR"},
                .mGrossInterest = 1'000'000,
                .mWithholding = {.mForeignTax = -1, .mBlankInExport = false},
                .mSourceCountry = "FR",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mField, "Value");
    EXPECT_EQ(result.mDiagnostics.at(0).mContext.mInstrumentName, "A Broker");
    EXPECT_FALSE(result.mDiagnostics.at(0).mContext.mIsin.has_value());
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mField, "ForeignTax");
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mInstrumentName, "B Issuer");
    EXPECT_EQ(result.mDiagnostics.at(1).mContext.mIsin, "FR0000000002");
}

TEST(ObrXmlWriterTest, TheWholeCouponIsWrittenWhateverWasPaidAsAccruedInterest) {
    // Accrued interest paid with a purchase is part of the cost, not of the coupon: a coupon of EUR
    // 50.0000 is written as 50.00.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{
        .mRows = {
            {
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "FR0000000001"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "A Issuer", .mCountry = "FR"},
                .mGrossInterest = 500'000,
                .mWithholding = {.mForeignTax = 0, .mBlankInExport = false},
                .mSourceCountry = "FR",
                .mTreatyClaim = std::nullopt,
            },
        }};

    const auto result = ObrXmlWriter::write(metadata, report);

    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(textsOf(*result.mXml, "Value"), std::vector<std::string>{"50.00"});
}

TEST(ObrXmlWriterTest, YearBelow1000IsRejectedAsPeriod) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 999,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{.mRows = {}};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "Period", std::nullopt, std::nullopt);
}

TEST(ObrXmlWriterTest, TaxNumberWithSevenDigitsIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "1234567", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{.mRows = {}};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "taxNumber", std::nullopt, std::nullopt);
}

TEST(ObrXmlWriterTest, ThePhoneNumberOfTheTaxpayerMustBeValidXmlText) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true, .mPhoneNumber = "1\x03"},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };
    const OtherInterestReportObr report{.mRows = {}};

    const auto result = ObrXmlWriter::write(metadata, report);

    expectOneError(result, "TelephoneNumber", std::nullopt, std::nullopt);
}

TEST(ObrXmlWriterTest, SchemaRejectsAValueWithThreeDecimals) {
    // Proves that the schema check can fail: Value has two decimals at most.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_Obr>
      <Period>2025</Period>
      <Interest>
        <Date>2025-03-31</Date>
        <Value>1.234</Value>
      </Interest>
    </Doh_Obr>
  </body>
</Envelope>
)xml";

    expectInvalidForObrSchema(xml);
}

} // namespace
