#include "../support/xsd_validation.hpp"
#include "utils/date_utils.hpp"
#include "xml/kdvp_xml_writer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;
using taxbroker::test::expectInvalidForKdvpSchemas;
using taxbroker::test::expectValidForKdvpSchemas;

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

/// For years that a calendar date in an export cannot have, such as 10000.
Date firstOfJanuary(int aYear) {
    return Date{std::chrono::sys_days{std::chrono::year{aYear} / 1 / 1}.time_since_epoch()};
}

std::string repeated(std::string_view aText, std::size_t aCount) {
    std::string text;

    for (std::size_t index = 0; index < aCount; ++index)
    {
        text += aText;
    }

    return text;
}

constexpr std::string_view kTaxNumberMessage =
    "The value of the XML field taxNumber is outside what the FURS form allows: wrong length or "
    "format, a negative number, or characters the form does not accept. No XML file was written "
    "for this form.";

TEST(KdvpXmlWriterTest, OneSaleUsingTwoPurchasesWritesTheWholeDocument) {
    // Example "one sale uses two purchases" in tax_rules.md.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 1'000'000'000,     // 10 units
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 1'000'000'000}, // 10
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 1},
                             .mDate = day("2024-02-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 500'000'000,       // 5 units
                             .mUnitPrice = 2'000'000'000, // EUR 20
                         },
                     .mStock = 1'500'000'000}, // 10 + 5
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 2},
                             .mDate = day("2024-03-10"),
                             .mUnits = 1'500'000'000,     // 15 units
                             .mUnitPrice = 3'000'000'000, // EUR 30
                             .mLossRule = KdvpLossRule::NotBlocked,
                         },
                     .mStock = 0}, // 15 - 15
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>1</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
      <KDVPItem>
        <ItemID>1</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000001</ISIN>
          <Name>Example Shares</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>10.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>10.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Purchase>
              <F1>2024-02-10</F1>
              <F2>B</F2>
              <F3>5.0000</F3>
              <F4>20.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>15.0000</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Sale>
              <F6>2024-03-10</F6>
              <F7>15.0000</F7>
              <F9>30.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, AllOptionalTaxpayerFieldsAreWrittenInSchemaOrder) {
    const ReportMetadata metadata{
        .mTaxpayer =
            {
                .mTaxNumber = "12345678",
                .mName = "Test Taxpayer",
                .mAddress = "Test Street 1",
                .mCity = "Test City",
                .mPostCode = "1000",
                .mPostName = "Test City",
                .mBirthDate = day("1980-02-29"),
                .mIsResident = true,
                .mEmail = "taxpayer@example.com",
                .mPhoneNumber = "+38600000000",
            },
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // The header follows taxPayerType of EDP-Common-1.xsd. TelephoneNumber comes after
    // IsResident and Email after ShareCount, as in the KDVP element of Doh_KDVP_9.xsd.
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
      <edp:name>Test Taxpayer</edp:name>
      <edp:address1>Test Street 1</edp:address1>
      <edp:city>Test City</edp:city>
      <edp:postNumber>1000</edp:postNumber>
      <edp:postName>Test City</edp:postName>
      <edp:birthDate>1980-02-29</edp:birthDate>
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <TelephoneNumber>+38600000000</TelephoneNumber>
        <SecurityCount>0</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
        <Email>taxpayer@example.com</Email>
      </KDVP>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, MissingOptionalFieldsAreLeftOut) {
    // No name for the list. The writer never adds Code, KDVPItem/Name or F11. F5 (0.0000 on the
    // purchase) and DocumentWorkflowName are always written, as in the legacy output.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 1},
                             .mDate = day("2024-02-10"),
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'200'000'000, // EUR 12
                             .mLossRule = KdvpLossRule::NotBlocked,
                         },
                     .mStock = 0}, // 1 - 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>1</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
      <KDVPItem>
        <ItemID>1</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000001</ISIN>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>1.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>1.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-02-10</F6>
              <F7>1.0000</F7>
              <F9>12.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, SelfReportWritesItsWorkflowCode) {
    // S and the name SelfReport in the header and in KDVP, as the legacy GUI wrote for
    // "Self-Report (Samoprijava)".
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::SelfReport,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
      <edp:resident>true</edp:resident>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>S</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>SelfReport</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>S</DocumentWorkflowID>
        <DocumentWorkflowName>SelfReport</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>0</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, NonResidentWritesFalseInBothPlaces) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = false},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
      <edp:resident>false</edp:resident>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>false</IsResident>
        <SecurityCount>0</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, QuantitiesKeepFourToEightDecimals) {
    // calculations.md, "XML precision": write the exact units; remove trailing zeros only down to
    // four decimals. Every unit value is EUR 10 or EUR 12.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP
        report{
            .mLists =
                {
                    {
                        .mIsin = "XX0000000001",
                        .mName = "Example Shares",
                        .mIsFund = false,
                        .mRows =
                            {
                                {.mEntry =
                                     KdvpPurchaseRow{
                                         .mPurchase = {StatementEventKind::Trade, 0, 0},
                                         .mDate = day("2024-01-10"),
                                         .mMethod = KdvpPurchaseMethod::B,
                                         .mUnits = 200'000'000,       // 2 units
                                         .mUnitPrice = 1'000'000'000, // EUR 10
                                     },
                                 .mStock = 200'000'000}, // 2
                                {.mEntry =
                                     KdvpPurchaseRow{
                                         .mPurchase = {StatementEventKind::Trade, 0, 1},
                                         .mDate = day("2024-01-11"),
                                         .mMethod = KdvpPurchaseMethod::B,
                                         .mUnits = 12'345'678,        // 0.12345678 units
                                         .mUnitPrice = 1'000'000'000, // EUR 10
                                     },
                                 .mStock = 212'345'678}, // 2 + 0.12345678
                                {.mEntry =
                                     KdvpSaleRow{
                                         .mSale = {StatementEventKind::Trade, 0, 2},
                                         .mDate = day("2024-02-10"),
                                         .mUnits = 12'345'678,        // 0.12345678 units
                                         .mUnitPrice = 1'200'000'000, // EUR 12
                                         .mLossRule = KdvpLossRule::NotBlocked,
                                     },
                                 .mStock = 200'000'000}, // 2.12345678 - 0.12345678
                                {.mEntry =
                                     KdvpSaleRow{
                                         .mSale = {StatementEventKind::Trade, 0, 3},
                                         .mDate = day("2024-02-11"),
                                         .mUnits = 50'000'000,        // 0.5 units
                                         .mUnitPrice = 1'200'000'000, // EUR 12
                                         .mLossRule = KdvpLossRule::NotBlocked,
                                     },
                                 .mStock = 150'000'000}, // 2 - 0.5
                                {.mEntry =
                                     KdvpSaleRow{
                                         .mSale = {StatementEventKind::Trade, 0, 4},
                                         .mDate = day("2024-02-12"),
                                         .mUnits = 26'550'000,        // 0.2655 units
                                         .mUnitPrice = 1'200'000'000, // EUR 12
                                         .mLossRule = KdvpLossRule::NotBlocked,
                                     },
                                 .mStock = 123'450'000}, // 1.5 - 0.2655
                                {.mEntry =
                                     KdvpPurchaseRow{
                                         .mPurchase = {StatementEventKind::Trade, 0, 5},
                                         .mDate = day("2024-03-10"),
                                         .mMethod = KdvpPurchaseMethod::B,
                                         .mUnits = 6'000,             // 0.00006 units
                                         .mUnitPrice = 1'000'000'000, // EUR 10
                                     },
                                 .mStock = 123'456'000}, // 1.2345 + 0.00006
                                {.mEntry =
                                     KdvpPurchaseRow{
                                         .mPurchase = {StatementEventKind::Trade, 0, 6},
                                         .mDate = day("2024-03-11"),
                                         .mMethod = KdvpPurchaseMethod::B,
                                         .mUnits = 1,                 // 0.00000001 units
                                         .mUnitPrice = 1'000'000'000, // EUR 10
                                     },
                                 .mStock = 123'456'001}, // 1.23456 + 0.00000001
                                {.mEntry =
                                     KdvpSaleRow{
                                         .mSale = {StatementEventKind::Trade, 0, 7},
                                         .mDate = day("2024-04-10"),
                                         .mUnits = 123'456'001,       // 1.23456001 units
                                         .mUnitPrice = 1'200'000'000, // EUR 12
                                         .mLossRule = KdvpLossRule::NotBlocked,
                                     },
                                 .mStock = 0}, // 1.23456001 - 1.23456001
                            },
                    }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>1</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
      <KDVPItem>
        <ItemID>1</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000001</ISIN>
          <Name>Example Shares</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>2.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>2.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Purchase>
              <F1>2024-01-11</F1>
              <F2>B</F2>
              <F3>0.12345678</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>2.12345678</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Sale>
              <F6>2024-02-10</F6>
              <F7>0.12345678</F7>
              <F9>12.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>2.0000</F8>
          </Row>
          <Row>
            <ID>3</ID>
            <Sale>
              <F6>2024-02-11</F6>
              <F7>0.5000</F7>
              <F9>12.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>1.5000</F8>
          </Row>
          <Row>
            <ID>4</ID>
            <Sale>
              <F6>2024-02-12</F6>
              <F7>0.2655</F7>
              <F9>12.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>1.2345</F8>
          </Row>
          <Row>
            <ID>5</ID>
            <Purchase>
              <F1>2024-03-10</F1>
              <F2>B</F2>
              <F3>0.00006</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>1.23456</F8>
          </Row>
          <Row>
            <ID>6</ID>
            <Purchase>
              <F1>2024-03-11</F1>
              <F2>B</F2>
              <F3>0.00000001</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>1.23456001</F8>
          </Row>
          <Row>
            <ID>7</ID>
            <Sale>
              <F6>2024-04-10</F6>
              <F7>1.23456001</F7>
              <F9>12.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, UnitValuesKeepFourToEightDecimals) {
    // calculations.md, "XML precision": write UnitPrice without changing its value; remove
    // trailing zeros only down to four decimals.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP
        report{.mLists =
                   {
                       {
                           .mIsin = "XX0000000001",
                           .mName = "Example Shares",
                           .mIsFund = false,
                           .mRows =
                               {
                                   {.mEntry =
                                        KdvpPurchaseRow{
                                            .mPurchase = {StatementEventKind::Trade, 0, 0},
                                            .mDate = day("2024-01-10"),
                                            .mMethod = KdvpPurchaseMethod::B,
                                            .mUnits = 100'000'000,       // 1 unit
                                            .mUnitPrice = 1'234'565'000, // EUR 12.34565
                                        },
                                    .mStock = 100'000'000}, // 1
                                   {.mEntry =
                                        KdvpPurchaseRow{
                                            .mPurchase = {StatementEventKind::Trade, 0, 1},
                                            .mDate = day("2024-01-11"),
                                            .mMethod = KdvpPurchaseMethod::B,
                                            .mUnits = 100'000'000, // 1 unit
                                            .mUnitPrice = 1,       // EUR 0.00000001
                                        },
                                    .mStock = 200'000'000}, // 1 + 1
                                   {.mEntry =
                                        KdvpSaleRow{
                                            .mSale = {StatementEventKind::Trade, 0, 2},
                                            .mDate = day("2024-02-10"),
                                            .mUnits = 100'000'000,       // 1 unit
                                            .mUnitPrice = 1'000'000'000, // EUR 10
                                            .mLossRule = KdvpLossRule::NotBlocked,
                                        },
                                    .mStock = 100'000'000}, // 2 - 1
                                   {.mEntry =
                                        KdvpSaleRow{
                                            .mSale = {StatementEventKind::Trade, 0, 3},
                                            .mDate = day("2024-02-11"),
                                            .mUnits = 100'000'000,       // 1 unit
                                            .mUnitPrice = 1'234'567'891, // EUR 12.34567891
                                            .mLossRule = KdvpLossRule::NotBlocked,
                                        },
                                    .mStock = 0}, // 1 - 1
                               },
                       }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>1</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
      <KDVPItem>
        <ItemID>1</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000001</ISIN>
          <Name>Example Shares</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>1.0000</F3>
              <F4>12.34565</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>1.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Purchase>
              <F1>2024-01-11</F1>
              <F2>B</F2>
              <F3>1.0000</F3>
              <F4>0.00000001</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>2.0000</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Sale>
              <F6>2024-02-10</F6>
              <F7>1.0000</F7>
              <F9>10.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>1.0000</F8>
          </Row>
          <Row>
            <ID>3</ID>
            <Sale>
              <F6>2024-02-11</F6>
              <F7>1.0000</F7>
              <F9>12.34567891</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, LargestStoredValuesFitTheSchemaDigits) {
    // The largest int64 at eight decimals has 11 integer digits; F3, F7 and F8 allow 12 and F4
    // and F9 allow 14, so no stored value is too large for the form.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 9'223'372'036'854'775'807,     // 92233720368.54775807 units
                             .mUnitPrice = 9'223'372'036'854'775'807, // EUR 92233720368.54775807
                         },
                     .mStock = 9'223'372'036'854'775'807}, // 92233720368.54775807
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 1},
                             .mDate = day("2024-02-10"),
                             .mUnits = 9'223'372'036'854'775'807,     // 92233720368.54775807 units
                             .mUnitPrice = 9'223'372'036'854'775'807, // EUR 92233720368.54775807
                             .mLossRule = KdvpLossRule::NotBlocked,
                         },
                     .mStock = 0},
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>1</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
      <KDVPItem>
        <ItemID>1</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000001</ISIN>
          <Name>Example Shares</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>92233720368.54775807</F3>
              <F4>92233720368.54775807</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>92233720368.54775807</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-02-10</F6>
              <F7>92233720368.54775807</F7>
              <F9>92233720368.54775807</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, TextIsEscapedAndNonAsciiIsKept) {
    // &, < and > are escaped; quotes need no escaping in element text. Slovenian letters stay
    // UTF-8 text.
    const ReportMetadata metadata{
        .mTaxpayer =
            {
                .mTaxNumber = "12345678",
                .mName = "Testni Zavezanec Čšž",
                .mIsResident = true,
            },
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {
            {
                .mIsin = "XX0000000001",
                .mName = R"(Example & Sons <Class A> "Pref")",
                .mIsFund = false,
                .mRows =
                    {
                        {.mEntry =
                             KdvpPurchaseRow{
                                 .mPurchase = {StatementEventKind::Trade, 0, 0},
                                 .mDate = day("2024-01-10"),
                                 .mMethod = KdvpPurchaseMethod::B,
                                 .mUnits = 100'000'000,       // 1 unit
                                 .mUnitPrice = 1'000'000'000, // EUR 10
                             },
                         .mStock = 100'000'000}, // 1
                        {.mEntry =
                             KdvpSaleRow{
                                 .mSale = {StatementEventKind::Trade, 0, 1},
                                 .mDate = day("2024-02-10"),
                                 .mUnits = 100'000'000,       // 1 unit
                                 .mUnitPrice = 1'200'000'000, // EUR 12
                                 .mLossRule = KdvpLossRule::NotBlocked,
                             },
                         .mStock = 0}, // 1 - 1
                    },
            },
            {
                .mIsin = "XX0000000002",
                .mName = "Primer Delnice Čšž",
                .mIsFund = false,
                .mRows =
                    {
                        {.mEntry =
                             KdvpPurchaseRow{
                                 .mPurchase = {StatementEventKind::Trade, 1, 0},
                                 .mDate = day("2024-01-10"),
                                 .mMethod = KdvpPurchaseMethod::B,
                                 .mUnits = 100'000'000,       // 1 unit
                                 .mUnitPrice = 1'000'000'000, // EUR 10
                             },
                         .mStock = 100'000'000}, // 1
                        {.mEntry =
                             KdvpSaleRow{
                                 .mSale = {StatementEventKind::Trade, 1, 1},
                                 .mDate = day("2024-02-10"),
                                 .mUnits = 100'000'000,       // 1 unit
                                 .mUnitPrice = 1'200'000'000, // EUR 12
                                 .mLossRule = KdvpLossRule::NotBlocked,
                             },
                         .mStock = 0}, // 1 - 1
                    },
            },
        }};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
      <edp:name>Testni Zavezanec Čšž</edp:name>
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>2</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
      <KDVPItem>
        <ItemID>1</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000001</ISIN>
          <Name>Example &amp; Sons &lt;Class A&gt; "Pref"</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>1.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>1.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-02-10</F6>
              <F7>1.0000</F7>
              <F9>12.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
      <KDVPItem>
        <ItemID>2</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000002</ISIN>
          <Name>Primer Delnice Čšž</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>1.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>1.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-02-10</F6>
              <F7>1.0000</F7>
              <F9>12.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, CharactersOfThreeAndFourBytesAreKept) {
    // € is three bytes in UTF-8 (U+20AC) and 𐍈 is four (U+10348).
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example € Fund 𐍈",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 1},
                             .mDate = day("2024-02-10"),
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'200'000'000, // EUR 12
                             .mLossRule = KdvpLossRule::NotBlocked,
                         },
                     .mStock = 0}, // 1 - 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>1</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
      <KDVPItem>
        <ItemID>1</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000001</ISIN>
          <Name>Example € Fund 𐍈</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>1.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>1.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-02-10</F6>
              <F7>1.0000</F7>
              <F9>12.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, NameOfOneHundredCharactersIsAccepted) {
    // The limit is 100 characters, not bytes: 100 Ž are 200 bytes.
    const std::string hundredCharacters = repeated("Ž", 100);
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = hundredCharacters,
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 1},
                             .mDate = day("2024-02-10"),
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'200'000'000, // EUR 12
                             .mLossRule = KdvpLossRule::NotBlocked,
                         },
                     .mStock = 0}, // 1 - 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_EQ(hundredCharacters.size(), 200U);
    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml,
              std::string{R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>1</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
      <KDVPItem>
        <ItemID>1</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000001</ISIN>
          <Name>)xml"} +
                  hundredCharacters + std::string{R"xml(</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>1.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>1.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-02-10</F6>
              <F7>1.0000</F7>
              <F9>12.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
    </Doh_KDVP>
  </body>
</Envelope>
)xml"});

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, ListsAndRowsKeepModelOrderAndNumbering) {
    // The model lists XX0000000002 first. The writer keeps that order and does not sort.
    // ItemID counts from 1; the row ID counts from 0 in each list.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {
            {
                .mIsin = "XX0000000002",
                .mName = "Example Fund",
                .mIsFund = true,
                .mRows =
                    {
                        {.mEntry =
                             KdvpPurchaseRow{
                                 .mPurchase = {StatementEventKind::Trade, 1, 0},
                                 .mDate = day("2024-01-10"),
                                 .mMethod = KdvpPurchaseMethod::B,
                                 .mUnits = 200'000'000,       // 2 units
                                 .mUnitPrice = 5'000'000'000, // EUR 50
                             },
                         .mStock = 200'000'000}, // 2
                        {.mEntry =
                             KdvpSaleRow{
                                 .mSale = {StatementEventKind::Trade, 1, 1},
                                 .mDate = day("2024-02-10"),
                                 .mUnits = 200'000'000,       // 2 units
                                 .mUnitPrice = 6'000'000'000, // EUR 60
                                 .mLossRule = KdvpLossRule::NotBlocked,
                             },
                         .mStock = 0}, // 2 - 2
                    },
            },
            {
                .mIsin = "XX0000000001",
                .mName = "Example Shares",
                .mIsFund = false,
                .mRows =
                    {
                        {.mEntry =
                             KdvpPurchaseRow{
                                 .mPurchase = {StatementEventKind::Trade, 0, 0},
                                 .mDate = day("2024-03-10"),
                                 .mMethod = KdvpPurchaseMethod::B,
                                 .mUnits = 300'000'000,       // 3 units
                                 .mUnitPrice = 1'000'000'000, // EUR 10
                             },
                         .mStock = 300'000'000}, // 3
                        {.mEntry =
                             KdvpSaleRow{
                                 .mSale = {StatementEventKind::Trade, 0, 1},
                                 .mDate = day("2024-04-10"),
                                 .mUnits = 300'000'000,       // 3 units
                                 .mUnitPrice = 1'100'000'000, // EUR 11
                                 .mLossRule = KdvpLossRule::NotBlocked,
                             },
                         .mStock = 0}, // 3 - 3
                    },
            },
        }};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>2</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
      <KDVPItem>
        <ItemID>1</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000002</ISIN>
          <Name>Example Fund</Name>
          <IsFond>true</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>2.0000</F3>
              <F4>50.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>2.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-02-10</F6>
              <F7>2.0000</F7>
              <F9>60.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
      <KDVPItem>
        <ItemID>2</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000001</ISIN>
          <Name>Example Shares</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-03-10</F1>
              <F2>B</F2>
              <F3>3.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>3.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-04-10</F6>
              <F7>3.0000</F7>
              <F9>11.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, EverySaleRowHasF10AndPurchaseRowsHaveNone) {
    // A profitable sale gets F10 true: nothing blocks its gain.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 400'000'000,       // 4 units
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 400'000'000}, // 4
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 1},
                             .mDate = day("2024-02-10"),
                             .mUnits = 400'000'000,       // 4 units
                             .mUnitPrice = 2'500'000'000, // EUR 25
                             .mLossRule = KdvpLossRule::NotBlocked,
                         },
                     .mStock = 0}, // 4 - 4
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>1</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
      <KDVPItem>
        <ItemID>1</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000001</ISIN>
          <Name>Example Shares</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>4.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>4.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-02-10</F6>
              <F7>4.0000</F7>
              <F9>25.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, EligibleAndBlockedPartsOfOneSaleAreTwoSaleRows) {
    // One loss-making sale of 10 units: the loss rule (#220) lets 6 units reduce gains and blocks
    // 4. Both rows share the source sale. The writer writes them as two rows with the model's F8,
    // and merges or recalculates nothing.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 1'000'000'000,     // 10 units
                             .mUnitPrice = 2'000'000'000, // EUR 20
                         },
                     .mStock = 1'000'000'000}, // 10
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 1},
                             .mDate = day("2024-03-10"),
                             .mUnits = 600'000'000,       // 6 units
                             .mUnitPrice = 1'500'000'000, // EUR 15
                             .mLossRule = KdvpLossRule::NotBlocked,
                         },
                     .mStock = 400'000'000}, // 10 - 6
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 1},
                             .mDate = day("2024-03-10"),
                             .mUnits = 400'000'000,       // 4 units
                             .mUnitPrice = 1'500'000'000, // EUR 15
                             .mLossRule = KdvpLossRule::Blocked,
                         },
                     .mStock = 0}, // 4 - 4
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>1</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
      <KDVPItem>
        <ItemID>1</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000001</ISIN>
          <Name>Example Shares</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>10.0000</F3>
              <F4>20.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>10.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-03-10</F6>
              <F7>6.0000</F7>
              <F9>15.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>4.0000</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Sale>
              <F6>2024-03-10</F6>
              <F7>4.0000</F7>
              <F9>15.0000</F9>
              <F10>false</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, EmptyReportWritesNoLists) {
    // A year without sales. The writer still writes a valid document; the caller decides that
    // the form has no data and writes no file.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>0</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
    </Doh_KDVP>
  </body>
</Envelope>
)xml");

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlWriterTest, SchemaValidationRejectsAnUnknownF2Code) {
    // The expected document of OneSaleUsingTwoPurchasesWritesTheWholeDocument with F2 Z, a code
    // that Doh_KDVP_9.xsd does not have. Proves that the schema check can fail.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
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
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>O</DocumentWorkflowID>
        <DocumentWorkflowName>Original</DocumentWorkflowName>
        <Year>2024</Year>
        <PeriodStart>2024-01-01</PeriodStart>
        <PeriodEnd>2024-12-31</PeriodEnd>
        <IsResident>true</IsResident>
        <SecurityCount>1</SecurityCount>
        <SecurityShortCount>0</SecurityShortCount>
        <SecurityWithContractCount>0</SecurityWithContractCount>
        <SecurityWithContractShortCount>0</SecurityWithContractShortCount>
        <ShareCount>0</ShareCount>
      </KDVP>
      <KDVPItem>
        <ItemID>1</ItemID>
        <InventoryListType>PLVP</InventoryListType>
        <Securities>
          <ISIN>XX0000000001</ISIN>
          <Name>Example Shares</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>Z</F2>
              <F3>10.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>10.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Purchase>
              <F1>2024-02-10</F1>
              <F2>Z</F2>
              <F3>5.0000</F3>
              <F4>20.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>15.0000</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Sale>
              <F6>2024-03-10</F6>
              <F7>15.0000</F7>
              <F9>30.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
        </Securities>
      </KDVPItem>
    </Doh_KDVP>
  </body>
</Envelope>
)xml";

    expectInvalidForKdvpSchemas(xml);
}

TEST(KdvpXmlWriterTest, TaxNumberWithSevenDigitsIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "1234567", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports, std::vector{ReportType::CapitalGains});
    EXPECT_EQ(diagnostic.mContext.mField, "taxNumber");
    EXPECT_FALSE(diagnostic.mContext.mIsin.has_value());
    EXPECT_FALSE(diagnostic.mContext.mInstrumentName.has_value());
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "taxNumber")});
    // The message names the field, never the tax number.
    EXPECT_EQ(diagnostic.mMessage, kTaxNumberMessage);
}

TEST(KdvpXmlWriterTest, TaxNumberWithALetterIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "1234567A", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports, std::vector{ReportType::CapitalGains});
    EXPECT_EQ(diagnostic.mContext.mField, "taxNumber");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "taxNumber")});
    EXPECT_EQ(diagnostic.mMessage, kTaxNumberMessage);
}

TEST(KdvpXmlWriterTest, TaxNumberWithASignIsRejected) {
    // Eight characters, but "-" is not a digit.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "-1234567", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mField, "taxNumber");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "taxNumber")});
    EXPECT_EQ(diagnostic.mMessage, kTaxNumberMessage);
}

TEST(KdvpXmlWriterTest, TaxNumberOfZerosIsRejected) {
    // EDP-Common-1.xsd: eight digits and a positive integer, so 00000000 is not a tax number.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "00000000", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports, std::vector{ReportType::CapitalGains});
    EXPECT_EQ(diagnostic.mContext.mField, "taxNumber");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "taxNumber")});
    EXPECT_EQ(diagnostic.mMessage, kTaxNumberMessage);
}

TEST(KdvpXmlWriterTest, YearBelow1000IsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 999,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports, std::vector{ReportType::CapitalGains});
    EXPECT_EQ(diagnostic.mContext.mField, "Year");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "Year")});
    EXPECT_EQ(diagnostic.mMessage,
              "The value of the XML field Year is outside what the FURS form allows: wrong length "
              "or format, a negative number, or characters the form does not accept. No XML file "
              "was written for this form.");
}

TEST(KdvpXmlWriterTest, YearAbove9999IsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 10000,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mField, "Year");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "Year")});
}

TEST(KdvpXmlWriterTest, BirthDateInYearZeroIsRejected) {
    // The parser reads 0000-01-01 as a calendar date, but xs:date has no year 0000.
    const ReportMetadata metadata{
        .mTaxpayer =
            {
                .mTaxNumber = "12345678",
                .mBirthDate = day("0000-01-01"),
                .mIsResident = true,
            },
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mField, "birthDate");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "birthDate")});
}

TEST(KdvpXmlWriterTest, TaxpayerNameWithInvalidUtf8IsRejected) {
    // 0xC3 starts a two-byte character, but "(" (0x28) cannot continue it.
    const ReportMetadata metadata{
        .mTaxpayer =
            {
                .mTaxNumber = "12345678",
                .mName = "Test \xC3\x28 Taxpayer",
                .mIsResident = true,
            },
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mField, "name");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "name")});
}

TEST(KdvpXmlWriterTest, PostNumberOfThirteenCharactersIsRejected) {
    // PostNumberType of EDP-Common-1.xsd allows at most 12 characters.
    const ReportMetadata metadata{
        .mTaxpayer =
            {
                .mTaxNumber = "12345678",
                .mPostCode = "1234567890123",
                .mIsResident = true,
            },
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{.mLists = {}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mField, "postNumber");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "postNumber")});
}

TEST(KdvpXmlWriterTest, IsinOfThirteenCharactersIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX00000000012",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports, std::vector{ReportType::CapitalGains});
    EXPECT_EQ(diagnostic.mContext.mField, "ISIN");
    // The context names the list, so the user can find it.
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX00000000012");
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Example Shares");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "ISIN")});
    EXPECT_EQ(diagnostic.mMessage,
              "The value of the XML field ISIN is outside what the FURS form allows: wrong length "
              "or format, a negative number, or characters the form does not accept. No XML file "
              "was written for this form.");
}

TEST(KdvpXmlWriterTest, EmptyIsinIsRejected) {
    // The schema accepts an empty ISIN; a list without one cannot be filed.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mField, "ISIN");
    EXPECT_EQ(diagnostic.mContext.mIsin, "");
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Example Shares");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "ISIN")});
}

TEST(KdvpXmlWriterTest, IsinWithInvalidUtf8IsRejected) {
    // 0xC3 starts a two-byte character, but "(" (0x28) cannot continue it.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX000000\xC3\x28",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mField, "ISIN");
    // The broken ISIN is not copied into the diagnostic; the name still names the list.
    EXPECT_FALSE(diagnostic.mContext.mIsin.has_value());
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Example Shares");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "ISIN")});
}

TEST(KdvpXmlWriterTest, NameOfOneHundredAndOneCharactersIsRejected) {
    // Never shortened to fit: the list fails instead.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = repeated("A", 101),
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mField, "Name");
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "Name")});
}

TEST(KdvpXmlWriterTest, NameWithAControlCharacterIsRejected) {
    // U+0001 is not allowed in XML 1.0 text.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Bad\x01Name",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mField, "Name");
    // The broken name is not copied into the diagnostic; the ISIN still names the list.
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_FALSE(diagnostic.mContext.mInstrumentName.has_value());
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "Name")});
}

TEST(KdvpXmlWriterTest, NameWithABrokenTwoByteCharacterIsRejected) {
    // 0xC3 starts a two-byte character, but "(" (0x28) cannot continue it.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "\xC3\x28",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mField, "Name");
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_FALSE(diagnostic.mContext.mInstrumentName.has_value());
}

TEST(KdvpXmlWriterTest, NameWithAByteThatStartsNoCharacterIsRejected) {
    // 0xFF never appears in UTF-8.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example \xFF",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mContext.mField, "Name");
}

TEST(KdvpXmlWriterTest, NameEndingInsideACharacterIsRejected) {
    // 0xC5 starts a two-byte character (as in Ž, 0xC5 0xBD), but the text ends after it.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example \xC5",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mContext.mField, "Name");
}

TEST(KdvpXmlWriterTest, NameWithATooLongEncodingIsRejected) {
    // 0xC0 0xAF is "/" (U+002F) written with two bytes instead of one, which UTF-8 forbids.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example \xC0\xAF",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mContext.mField, "Name");
}

TEST(KdvpXmlWriterTest, NameWithASurrogateIsRejected) {
    // 0xED 0xA0 0x80 is U+D800, half of a UTF-16 pair, which UTF-8 forbids.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example \xED\xA0\x80",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mContext.mField, "Name");
}

TEST(KdvpXmlWriterTest, NameAboveTheLastCodePointIsRejected) {
    // 0xF4 0x90 0x80 0x80 would be U+110000; Unicode ends at U+10FFFF.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example \xF4\x90\x80\x80",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mContext.mField, "Name");
}

TEST(KdvpXmlWriterTest, NameWithUFFFEIsRejected) {
    // 0xEF 0xBF 0xBE is U+FFFE, valid UTF-8 but not allowed in XML 1.0 text.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example \xEF\xBF\xBE",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mContext.mField, "Name");
}

TEST(KdvpXmlWriterTest, NameWithUFFFFIsRejected) {
    // 0xEF 0xBF 0xBF is U+FFFF, valid UTF-8 but not allowed in XML 1.0 text.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example \xEF\xBF\xBF",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mContext.mField, "Name");
}

TEST(KdvpXmlWriterTest, PurchaseDateInYearZeroIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("0000-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mField, "F1");
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Example Shares");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "F1")});
}

TEST(KdvpXmlWriterTest, SaleDateAfterYear9999IsRejected) {
    // A year that needs five digits does not fit the YYYY-MM-DD of the form.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 100'000'000}, // 1
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 1},
                             .mDate = firstOfJanuary(10000),
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'200'000'000, // EUR 12
                             .mLossRule = KdvpLossRule::NotBlocked,
                         },
                     .mStock = 0}, // 1 - 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mField, "F6");
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "F6")});
}

TEST(KdvpXmlWriterTest, NegativePurchaseUnitsAreRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = -1,                // -0.00000001 units
                             .mUnitPrice = 1'000'000'000, // EUR 10
                         },
                     .mStock = 0},
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mField, "F3");
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "F3")});
    // The message never contains the value.
    EXPECT_EQ(diagnostic.mMessage,
              "The value of the XML field F3 is outside what the FURS form allows: wrong length "
              "or format, a negative number, or characters the form does not accept. No XML file "
              "was written for this form.");
}

TEST(KdvpXmlWriterTest, NegativePurchaseUnitValueIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpPurchaseRow{
                             .mPurchase = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-01-10"),
                             .mMethod = KdvpPurchaseMethod::B,
                             .mUnits = 100'000'000, // 1 unit
                             .mUnitPrice = -1,      // EUR -0.00000001
                         },
                     .mStock = 100'000'000}, // 1
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mField, "F4");
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "F4")});
}

TEST(KdvpXmlWriterTest, NegativeSaleUnitsAreRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-02-10"),
                             .mUnits = -1,                // -0.00000001 units
                             .mUnitPrice = 1'200'000'000, // EUR 12
                             .mLossRule = KdvpLossRule::NotBlocked,
                         },
                     .mStock = 0},
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mField, "F7");
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "F7")});
}

TEST(KdvpXmlWriterTest, NegativeSaleUnitValueIsRejected) {
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-02-10"),
                             .mUnits = 100'000'000, // 1 unit
                             .mUnitPrice = -1,      // EUR -0.00000001
                             .mLossRule = KdvpLossRule::NotBlocked,
                         },
                     .mStock = 0},
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mField, "F9");
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "F9")});
}

TEST(KdvpXmlWriterTest, NegativeStockIsRejected) {
    // The schema allows a negative F8, but only the developer export (#188) may write one.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-02-10"),
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'200'000'000, // EUR 12
                             .mLossRule = KdvpLossRule::NotBlocked,
                         },
                     .mStock = -1}, // -0.00000001
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::XmlGeneration);
    EXPECT_EQ(diagnostic.mContext.mField, "F8");
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mMessageParams, std::vector{textParam("field", "F8")});
}

TEST(KdvpXmlWriterTest, SeveralProblemsGiveOneDiagnosticEach) {
    // A bad tax number and a negative stock: two errors, the header field first, and no XML.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "1234567", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "XX0000000001",
            .mName = "Example Shares",
            .mIsFund = false,
            .mRows =
                {
                    {.mEntry =
                         KdvpSaleRow{
                             .mSale = {StatementEventKind::Trade, 0, 0},
                             .mDate = day("2024-02-10"),
                             .mUnits = 100'000'000,       // 1 unit
                             .mUnitPrice = 1'200'000'000, // EUR 12
                             .mLossRule = KdvpLossRule::NotBlocked,
                         },
                     .mStock = -1}, // -0.00000001
                },
        }}};

    const auto result = KdvpXmlWriter::write(metadata, report);

    EXPECT_FALSE(result.mXml.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& taxNumber = result.mDiagnostics.at(0);

    EXPECT_EQ(taxNumber.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(taxNumber.mContext.mField, "taxNumber");
    EXPECT_FALSE(taxNumber.mContext.mIsin.has_value());

    const auto& stock = result.mDiagnostics.at(1);

    EXPECT_EQ(stock.mCode, ProcessingDiagnosticCode::XmlValueOutOfRange);
    EXPECT_EQ(stock.mContext.mField, "F8");
    EXPECT_EQ(stock.mContext.mIsin, "XX0000000001");
}

} // namespace
