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

} // namespace
