#include "../support/kdvp_row_fifo.hpp"
#include "../support/xsd_validation.hpp"
#include "parsers/traderepublic_parser.hpp"
#include "processors/capital_gains_report_builder.hpp"
#include "processors/fifo_matcher.hpp"
#include "xml/kdvp_xml_writer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using namespace taxbroker;
using taxbroker::test::expectValidForKdvpSchemas;
using taxbroker::test::fifoOverKdvpRows;

class TemporaryKdvpInputs {
  public:
    TemporaryKdvpInputs() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_kdvp_synthetic_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create synthetic KDVP directory"};
        }
    }

    TemporaryKdvpInputs(const TemporaryKdvpInputs&) = delete;
    TemporaryKdvpInputs& operator=(const TemporaryKdvpInputs&) = delete;

    ~TemporaryKdvpInputs() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    /// Writes exactly the synthetic rows supplied by each scenario.
    std::filesystem::path write(std::string_view aName,
                                std::initializer_list<std::string_view> aRows) const {
        const auto path = mDirectory / aName;
        std::ofstream output{path};

        output << "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,"
                  "amount,fee,tax,currency,original_amount,original_currency,fx_rate,description,"
                  "transaction_id,counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

        for (const auto row : aRows)
        {
            output << row << '\n';
        }

        output.close();

        if (!output)
        {
            throw std::runtime_error{"Unable to write synthetic KDVP CSV"};
        }

        return path;
    }

  private:
    std::filesystem::path mDirectory;
};

TEST(KdvpXmlIntegrationTest, TwoFilesWithASaleUsingTwoPurchasesWriteTheWholeDocument) {
    // Example "one sale uses two purchases" in tax_rules.md, with the purchases and the sale in
    // two files.
    constexpr std::string_view firstPurchase =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view secondPurchase =
        R"(2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,20,20,-400,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view sale =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,30,450,,,EUR,,,,,synthetic-sell,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };

    const TemporaryKdvpInputs files;
    const auto purchasesPath = files.write("purchases.csv", {firstPurchase, secondPurchase});
    const auto salesPath = files.write("sales.csv", {sale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(purchasesPath)},
                                            {1, parser.parse(salesPath, 1)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(fifo.mDiagnostics.empty());

    const auto built =
        CapitalGainsReportBuilder::build(prepared.mMerged.mStatement, *prepared.mLedger, fifo, {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
    EXPECT_TRUE(built.mLongHeld.empty());
    ASSERT_TRUE(built.mReport.has_value());

    const auto result = KdvpXmlWriter::write(metadata, *built.mReport);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // Row 1 lists only the 5 units of the 20 @ 20 purchase that the sale uses. F8 is the running
    // stock of the listed rows: 10, 10 + 5 = 15, 15 - 15 = 0.
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
          <Name>Synthetic Share</Name>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_EQ(fifoList.mIsin, "XX0000000001");
    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 10, 10 + 5 = 15, 15 - 15 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"10", "15", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"10", "15", "0"}));
    // 15 x 30 - (10 x 10 + 5 x 20) = 450 - 200 = 250.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"250"}));
}

TEST(KdvpXmlIntegrationTest, BlockedIsinLeftOutByTheUserIsNotInTheXml) {
    // XX0000000001 has a full history. XX0000000002 is sold without any purchase, so its FIFO
    // pool stops and the ISIN is blocked until the user agrees to leave it out.
    constexpr std::string_view purchase =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sale =
        R"(2024-05-01T09:00:00Z,2024-05-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,12,48,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view saleWithoutPurchase =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Other Share,XX0000000002,-1,12,12,,,EUR,,,,,synthetic-sell-2,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };

    const TemporaryKdvpInputs files;
    const auto historyPath = files.write("history.csv", {purchase, sale});
    const auto otherPath = files.write("other.csv", {saleWithoutPurchase});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(historyPath)},
                                            {1, parser.parse(otherPath, 1)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    ASSERT_EQ(fifo.mDiagnostics.size(), 1U);
    EXPECT_EQ(fifo.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(fifo.mDiagnostics.at(0).mContext.mIsin, "XX0000000002");

    // Without the user's agreement there is no report.
    const auto withoutExclusions =
        CapitalGainsReportBuilder::build(prepared.mMerged.mStatement, *prepared.mLedger, fifo, {});

    EXPECT_FALSE(withoutExclusions.mReport.has_value());
    EXPECT_TRUE(withoutExclusions.mExcluded.empty());
    ASSERT_EQ(withoutExclusions.mBlocked.size(), 1U);

    const auto& blocked = withoutExclusions.mBlocked.at(0);

    EXPECT_EQ(blocked.mIsin, "XX0000000002");
    EXPECT_EQ(blocked.mReason, KdvpBlockReason::FifoStopped);

    // The user agrees to leave XX0000000002 out.
    const auto withExclusion = CapitalGainsReportBuilder::build(prepared.mMerged.mStatement,
                                                                *prepared.mLedger,
                                                                fifo,
                                                                {"XX0000000002"});

    EXPECT_TRUE(withExclusion.mBlocked.empty());
    ASSERT_EQ(withExclusion.mExcluded.size(), 1U);
    EXPECT_EQ(withExclusion.mExcluded.at(0).mIsin, "XX0000000002");
    ASSERT_TRUE(withExclusion.mReport.has_value());

    const auto result = KdvpXmlWriter::write(metadata, *withExclusion.mReport);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // Row 0 lists only the 4 units the sale uses, not all 10. F8: 4, then 4 - 4 = 0.
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
          <Name>Synthetic Share</Name>
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
              <F6>2024-05-01</F6>
              <F7>4.0000</F7>
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
    EXPECT_EQ(result.mXml->find("XX0000000002"), std::string::npos);

    expectValidForKdvpSchemas(*result.mXml);
}

TEST(KdvpXmlIntegrationTest, PurchasePartlySoldInAnEarlierYearListsTheEarlierSale2023) {
    // Example "a purchase partly sold in an earlier year" in tax_rules.md, read for 2023.
    constexpr std::string_view purchase =
        R"(2022-03-01T09:00:00Z,2022-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,15,10,-150,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view firstSale =
        R"(2023-05-10T09:00:00Z,2023-05-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,12,48,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view secondSale =
        R"(2024-06-20T09:00:00Z,2024-06-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-6,14,84,,,EUR,,,,,synthetic-sell-2,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2023,
        .mFilingType = FilingType::Original,
    };

    const TemporaryKdvpInputs files;
    const auto historyPath = files.write("history.csv", {purchase, firstSale, secondSale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(historyPath)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2023, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(fifo.mDiagnostics.empty());

    const auto built =
        CapitalGainsReportBuilder::build(prepared.mMerged.mStatement, *prepared.mLedger, fifo, {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
    EXPECT_TRUE(built.mLongHeld.empty());
    ASSERT_TRUE(built.mReport.has_value());

    const auto result = KdvpXmlWriter::write(metadata, *built.mReport);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // Row 0 lists only the 4 of the 15 purchased units that the 2023 sale uses. The 2024 sale is
    // after the year and not listed.
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
        <Year>2023</Year>
        <PeriodStart>2023-01-01</PeriodStart>
        <PeriodEnd>2023-12-31</PeriodEnd>
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
          <Name>Synthetic Share</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2022-03-01</F1>
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
              <F6>2023-05-10</F6>
              <F7>4.0000</F7>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_EQ(fifoList.mIsin, "XX0000000001");
    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 4, then 4 - 4 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"4", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"4", "0"}));
    // 4 x (12 - 10) = 8.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"8"}));
}

TEST(KdvpXmlIntegrationTest, PurchasePartlySoldInAnEarlierYearListsOnlyThisYearsSale2024) {
    // Example "a purchase partly sold in an earlier year" in tax_rules.md, read for 2024.
    constexpr std::string_view purchase =
        R"(2022-03-01T09:00:00Z,2022-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,15,10,-150,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view firstSale =
        R"(2023-05-10T09:00:00Z,2023-05-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,12,48,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view secondSale =
        R"(2024-06-20T09:00:00Z,2024-06-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-6,14,84,,,EUR,,,,,synthetic-sell-2,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };

    const TemporaryKdvpInputs files;
    const auto historyPath = files.write("history.csv", {purchase, firstSale, secondSale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(historyPath)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(fifo.mDiagnostics.empty());

    const auto built =
        CapitalGainsReportBuilder::build(prepared.mMerged.mStatement, *prepared.mLedger, fifo, {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
    EXPECT_TRUE(built.mLongHeld.empty());
    ASSERT_TRUE(built.mReport.has_value());

    const auto result = KdvpXmlWriter::write(metadata, *built.mReport);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // The 2023 sale is not repeated. Row 0 lists the 6 units the 2024 sale uses: not the 15
    // purchased, and not the 11 left after 2023. 4 + 6 + 5 still held = 15.
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
          <Name>Synthetic Share</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2022-03-01</F1>
              <F2>B</F2>
              <F3>6.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>6.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-06-20</F6>
              <F7>6.0000</F7>
              <F9>14.0000</F9>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_EQ(fifoList.mIsin, "XX0000000001");
    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 6, then 6 - 6 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"6", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"6", "0"}));
    // 6 x (14 - 10) = 24.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"24"}));
}

TEST(KdvpXmlIntegrationTest, StockLeftAfterTheYearIsNotListed2024) {
    // Example "stock left after the year" in tax_rules.md, read for 2024.
    constexpr std::string_view firstPurchase =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view firstSale =
        R"(2024-05-01T09:00:00Z,2024-05-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,12,48,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view secondPurchase =
        R"(2024-08-01T09:00:00Z,2024-08-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,5,11,-55,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view secondSale =
        R"(2024-10-01T09:00:00Z,2024-10-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-8,13,104,,,EUR,,,,,synthetic-sell-2,,,,)";
    constexpr std::string_view thirdSale =
        R"(2025-01-15T09:00:00Z,2025-01-15,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,15,45,,,EUR,,,,,synthetic-sell-3,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };

    const TemporaryKdvpInputs files;
    const auto historyPath =
        files.write("history.csv",
                    {firstPurchase, firstSale, secondPurchase, secondSale, thirdSale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(historyPath)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(fifo.mDiagnostics.empty());

    const auto built =
        CapitalGainsReportBuilder::build(prepared.mMerged.mStatement, *prepared.mLedger, fifo, {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
    EXPECT_TRUE(built.mLongHeld.empty());
    ASSERT_TRUE(built.mReport.has_value());

    const auto result = KdvpXmlWriter::write(metadata, *built.mReport);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // The 2024-02-01 purchase is one row of 10 (4 + 6) although two sales use it. Row 2 lists
    // 2 of the 5 units bought 2024-08-01; the other 3 are still held on 31 December.
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
          <Name>Synthetic Share</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-02-01</F1>
              <F2>B</F2>
              <F3>10.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>10.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-05-01</F6>
              <F7>4.0000</F7>
              <F9>12.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>6.0000</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Purchase>
              <F1>2024-08-01</F1>
              <F2>B</F2>
              <F3>2.0000</F3>
              <F4>11.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>8.0000</F8>
          </Row>
          <Row>
            <ID>3</ID>
            <Sale>
              <F6>2024-10-01</F6>
              <F7>8.0000</F7>
              <F9>13.0000</F9>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_EQ(fifoList.mIsin, "XX0000000001");
    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 10, 10 - 4 = 6, 6 + 2 = 8, 8 - 8 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"10", "6", "8", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"10", "6", "8", "0"}));
    // 4 x (12 - 10) = 8, and 6 x (13 - 10) + 2 x (13 - 11) = 18 + 4 = 22: EUR 30 in total.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"8", "22"}));
}

TEST(KdvpXmlIntegrationTest, StockLeftAfterTheYearIsListedInTheNextYear2025) {
    // Example "stock left after the year" in tax_rules.md, read for 2025.
    constexpr std::string_view firstPurchase =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view firstSale =
        R"(2024-05-01T09:00:00Z,2024-05-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,12,48,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view secondPurchase =
        R"(2024-08-01T09:00:00Z,2024-08-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,5,11,-55,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view secondSale =
        R"(2024-10-01T09:00:00Z,2024-10-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-8,13,104,,,EUR,,,,,synthetic-sell-2,,,,)";
    constexpr std::string_view thirdSale =
        R"(2025-01-15T09:00:00Z,2025-01-15,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,15,45,,,EUR,,,,,synthetic-sell-3,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };

    const TemporaryKdvpInputs files;
    const auto historyPath =
        files.write("history.csv",
                    {firstPurchase, firstSale, secondPurchase, secondSale, thirdSale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(historyPath)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2025, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(fifo.mDiagnostics.empty());

    const auto built =
        CapitalGainsReportBuilder::build(prepared.mMerged.mStatement, *prepared.mLedger, fifo, {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
    EXPECT_TRUE(built.mLongHeld.empty());
    ASSERT_TRUE(built.mReport.has_value());

    const auto result = KdvpXmlWriter::write(metadata, *built.mReport);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // Only the 2025 sale and the 3 units of the 2024-08-01 purchase it uses. The 2024 sales and
    // the 2024-02-01 purchase are in the 2024 list.
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
        <Year>2025</Year>
        <PeriodStart>2025-01-01</PeriodStart>
        <PeriodEnd>2025-12-31</PeriodEnd>
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
          <Name>Synthetic Share</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-08-01</F1>
              <F2>B</F2>
              <F3>3.0000</F3>
              <F4>11.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>3.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2025-01-15</F6>
              <F7>3.0000</F7>
              <F9>15.0000</F9>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_EQ(fifoList.mIsin, "XX0000000001");
    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 3, then 3 - 3 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"3", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"3", "0"}));
    // 3 x (15 - 11) = 12.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"12"}));
}

TEST(KdvpXmlIntegrationTest, SaleOfOldAndNewUnitsListsOnlyTheNewUnit2025) {
    // Example in "Long-held investments" in tax_rules.md (sale in 2025, so 15 years). The doc
    // gives no prices; EUR 10, 20 and 30 are this test's choice. FIFO takes the 2 units from
    // 2005, which are past the 15 years, and 1 unit from 2020.
    constexpr std::string_view oldPurchase =
        R"(2005-04-01T09:00:00Z,2005-04-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view newPurchase =
        R"(2020-04-01T09:00:00Z,2020-04-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,20,-40,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view sale =
        R"(2025-06-02T09:00:00Z,2025-06-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,30,90,,,EUR,,,,,synthetic-sell-1,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };

    const TemporaryKdvpInputs files;
    const auto historyPath = files.write("history.csv", {oldPurchase, newPurchase, sale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(historyPath)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2025, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(fifo.mDiagnostics.empty());

    const auto built =
        CapitalGainsReportBuilder::build(prepared.mMerged.mStatement, *prepared.mLedger, fifo, {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
    ASSERT_TRUE(built.mReport.has_value());
    ASSERT_EQ(built.mLongHeld.size(), 1U);
    // The 2 old units of the sale are tax free: kept beside the report, not in the XML.
    EXPECT_EQ(built.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mUnits = 200'000'000, // 2 units
              }));

    const auto result = KdvpXmlWriter::write(metadata, *built.mReport);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // Two rows: purchase 2020-04-01 with F3 1, sale 2025-06-02 with F7 1. The 2005 purchase has
    // no row.
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
        <Year>2025</Year>
        <PeriodStart>2025-01-01</PeriodStart>
        <PeriodEnd>2025-12-31</PeriodEnd>
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
          <Name>Synthetic Share</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2020-04-01</F1>
              <F2>B</F2>
              <F3>1.0000</F3>
              <F4>20.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>1.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2025-06-02</F6>
              <F7>1.0000</F7>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_EQ(fifoList.mIsin, "XX0000000001");
    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 1, then 1 - 1 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"1", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"1", "0"}));
    // Only the listed unit: 1 x (30 - 20) = 10.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"10"}));
}

TEST(KdvpXmlIntegrationTest, TwoInstrumentsAreTwoListsInIsinOrder) {
    // XX0000000002 comes first in the file and trades first, but the lists are in ISIN order.
    constexpr std::string_view otherPurchase =
        R"(2024-01-05T09:00:00Z,2024-01-05,DEFAULT,TRADING,BUY,STOCK,Synthetic Other Share,XX0000000002,3,5,-15,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view otherSale =
        R"(2024-03-05T09:00:00Z,2024-03-05,DEFAULT,TRADING,SELL,STOCK,Synthetic Other Share,XX0000000002,-3,7,21,,,EUR,,,,,synthetic-sell-2,,,,)";
    constexpr std::string_view purchase =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sale =
        R"(2024-04-10T09:00:00Z,2024-04-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,15,30,,,EUR,,,,,synthetic-sell-1,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };

    const TemporaryKdvpInputs files;
    const auto historyPath = files.write("history.csv", {otherPurchase, otherSale, purchase, sale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(historyPath)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(fifo.mDiagnostics.empty());

    const auto built =
        CapitalGainsReportBuilder::build(prepared.mMerged.mStatement, *prepared.mLedger, fifo, {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
    EXPECT_TRUE(built.mLongHeld.empty());
    ASSERT_TRUE(built.mReport.has_value());

    const auto result = KdvpXmlWriter::write(metadata, *built.mReport);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // SecurityCount 2. ItemID 1 is XX0000000001, ItemID 2 is XX0000000002; the row ID starts
    // from 0 in each list.
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
          <ISIN>XX0000000001</ISIN>
          <Name>Synthetic Share</Name>
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
            <Sale>
              <F6>2024-04-10</F6>
              <F7>2.0000</F7>
              <F9>15.0000</F9>
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
          <Name>Synthetic Other Share</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-05</F1>
              <F2>B</F2>
              <F3>3.0000</F3>
              <F4>5.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>3.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-03-05</F6>
              <F7>3.0000</F7>
              <F9>7.0000</F9>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 2U);

    const auto& firstList = fifoLists.at(0);

    EXPECT_EQ(firstList.mIsin, "XX0000000001");
    EXPECT_TRUE(firstList.mProblems.empty());
    // F8: 2, then 2 - 2 = 0.
    EXPECT_EQ(firstList.mWrittenF8, (std::vector<std::string>{"2", "0"}));
    EXPECT_EQ(firstList.mCountedF8, (std::vector<std::string>{"2", "0"}));
    // 2 x (15 - 10) = 10.
    EXPECT_EQ(firstList.mSaleGains, (std::vector<std::string>{"10"}));

    const auto& secondList = fifoLists.at(1);

    EXPECT_EQ(secondList.mIsin, "XX0000000002");
    EXPECT_TRUE(secondList.mProblems.empty());
    // F8: 3, then 3 - 3 = 0.
    EXPECT_EQ(secondList.mWrittenF8, (std::vector<std::string>{"3", "0"}));
    EXPECT_EQ(secondList.mCountedF8, (std::vector<std::string>{"3", "0"}));
    // 3 x (7 - 5) = 6.
    EXPECT_EQ(secondList.mSaleGains, (std::vector<std::string>{"6"}));
}

TEST(KdvpXmlIntegrationTest, FractionalUnitsAndPricesKeepTheirDecimals2024) {
    // Fractional units, as in a savings plan. The sale takes 0.5 units from each purchase.
    constexpr std::string_view firstPurchase =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.5,100.1234,-50.0617,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view secondPurchase =
        R"(2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1.25,80.5,-100.625,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view sale =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,120.75,120.75,,,EUR,,,,,synthetic-sell-1,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };

    const TemporaryKdvpInputs files;
    const auto historyPath = files.write("history.csv", {firstPurchase, secondPurchase, sale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(historyPath)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(fifo.mDiagnostics.empty());

    const auto built =
        CapitalGainsReportBuilder::build(prepared.mMerged.mStatement, *prepared.mLedger, fifo, {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
    EXPECT_TRUE(built.mLongHeld.empty());
    ASSERT_TRUE(built.mReport.has_value());

    const auto result = KdvpXmlWriter::write(metadata, *built.mReport);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mXml.has_value());
    // Row 1 lists only the 0.5 of the 1.25 units bought 2024-02-10 that the sale uses; the other
    // 0.75 are still held.
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
          <Name>Synthetic Share</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-01-10</F1>
              <F2>B</F2>
              <F3>0.5000</F3>
              <F4>100.1234</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>0.5000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Purchase>
              <F1>2024-02-10</F1>
              <F2>B</F2>
              <F3>0.5000</F3>
              <F4>80.5000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>1.0000</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Sale>
              <F6>2024-03-10</F6>
              <F7>1.0000</F7>
              <F9>120.7500</F9>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_EQ(fifoList.mIsin, "XX0000000001");
    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 0.5, 0.5 + 0.5 = 1, 1 - 1 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"0.5", "1", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"0.5", "1", "0"}));
    // 1 x 120.75 - (0.5 x 100.1234 + 0.5 x 80.5) = 120.75 - (50.0617 + 40.25) = 30.4383.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"30.4383"}));
}

TEST(KdvpXmlIntegrationTest, SelfReportForAnEarlierYearWritesItsWorkflowCode2023) {
    // A 2023 sale reported late as a self-report: I and SamoPrijava in the header and in KDVP,
    // the rows the same as in an original return.
    constexpr std::string_view purchase =
        R"(2023-02-01T09:00:00Z,2023-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sale =
        R"(2023-07-03T09:00:00Z,2023-07-03,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,16,48,,,EUR,,,,,synthetic-sell-1,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2023,
        .mFilingType = FilingType::SelfReport,
    };

    const TemporaryKdvpInputs files;
    const auto historyPath = files.write("history.csv", {purchase, sale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(historyPath)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2023, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(fifo.mDiagnostics.empty());

    const auto built =
        CapitalGainsReportBuilder::build(prepared.mMerged.mStatement, *prepared.mLedger, fifo, {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
    EXPECT_TRUE(built.mLongHeld.empty());
    ASSERT_TRUE(built.mReport.has_value());

    const auto result = KdvpXmlWriter::write(metadata, *built.mReport);

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
      <edp:DocumentWorkflowID>I</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>SamoPrijava</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_KDVP>
      <KDVP>
        <DocumentWorkflowID>I</DocumentWorkflowID>
        <DocumentWorkflowName>SamoPrijava</DocumentWorkflowName>
        <Year>2023</Year>
        <PeriodStart>2023-01-01</PeriodStart>
        <PeriodEnd>2023-12-31</PeriodEnd>
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
          <Name>Synthetic Share</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2023-02-01</F1>
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
              <F6>2023-07-03</F6>
              <F7>3.0000</F7>
              <F9>16.0000</F9>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_EQ(fifoList.mIsin, "XX0000000001");
    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 3, then 3 - 3 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"3", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"3", "0"}));
    // 3 x (16 - 10) = 18.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"18"}));
}

TEST(KdvpXmlIntegrationTest, LossMakingSaleGivesNoReportAndNoXml) {
    // 5 x (15 - 20) = EUR -25 is a loss. Until the 30-day loss rule exists, its eligibility is
    // unresolved, so there is no report and nothing to write.
    constexpr std::string_view purchase =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,5,20,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sale =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-5,15,75,,,EUR,,,,,synthetic-sell-1,,,,)";

    const TemporaryKdvpInputs files;
    const auto historyPath = files.write("history.csv", {purchase, sale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(historyPath)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(fifo.mDiagnostics.empty());

    const auto built =
        CapitalGainsReportBuilder::build(prepared.mMerged.mStatement, *prepared.mLedger, fifo, {});

    EXPECT_FALSE(built.mReport.has_value());
    EXPECT_TRUE(built.mExcluded.empty());
    EXPECT_TRUE(built.mLongHeld.empty());
    ASSERT_EQ(built.mBlocked.size(), 1U);
    EXPECT_EQ(built.mBlocked.at(0),
              (KdvpBlockedIsin{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mReason = KdvpBlockReason::LossEligibilityUnresolved,
                  .mSales = {{StatementEventKind::Trade, 0, 1}},
              }));
}

} // namespace
