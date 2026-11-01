#include "../support/kdvp_row_fifo.hpp"
#include "../support/xsd_validation.hpp"
#include "parsers/traderepublic_parser.hpp"
#include "processors/capital_gains_report_builder.hpp"
#include "processors/fifo_matcher.hpp"
#include "processors/loss_eligibility.hpp"
#include "utils/date_utils.hpp"
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

class TemporaryHistoryInputs {
  public:
    TemporaryHistoryInputs() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_history_synthetic_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create synthetic history directory"};
        }
    }

    TemporaryHistoryInputs(const TemporaryHistoryInputs&) = delete;
    TemporaryHistoryInputs& operator=(const TemporaryHistoryInputs&) = delete;

    ~TemporaryHistoryInputs() {
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
            throw std::runtime_error{"Unable to write synthetic history CSV"};
        }

        return path;
    }

  private:
    std::filesystem::path mDirectory;
};

// tax_rules.md, "Example across several years": one investment from 2008 to 2025. The split row's
// shares value is evidence only; the confirmed ratio 2 / 1 (new / old) is what the tests apply.

TEST(HistoricalScenarioIntegrationTest, TheExampleFromTwoFilesWritesItsRows2024) {
    // The 2008 to 2023 rows are in one file, the rest in the next. The split on 2024-02-01 is
    // 2 / 1. 2008 lot: 10 - 4 = 6 left, 12 after the split. 2023 lot: 8, 16 after the split, EUR 20
    // a unit. The June sale of 20 uses the 12 old and 8 new units: 8 x (25 - 20) = EUR 40. The
    // December sale of 4 new units at EUR 15 loses 4 x (15 - 20) = EUR 20, and the purchase of 4
    // units on 2025-01-05 (16 days later) blocks it: F10 false. The purchase row lists the 12 units
    // the two sales use (8 + 4); the 12 old units are past 15 years and not listed.
    constexpr std::string_view purchase2008 =
        R"(2008-05-10T09:00:00Z,2008-05-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,5,-50,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sale2023 =
        R"(2023-03-01T09:00:00Z,2023-03-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,6,24,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view purchase2023 =
        R"(2023-09-01T09:00:00Z,2023-09-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,8,40,-320,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view split2024 =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,12,,,,,,,,,,synthetic-split-1,,,,)";
    constexpr std::string_view juneSale =
        R"(2024-06-20T09:00:00Z,2024-06-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-20,25,500,,,EUR,,,,,synthetic-sell-2,,,,)";
    constexpr std::string_view decemberSale =
        R"(2024-12-20T09:00:00Z,2024-12-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,15,60,,,EUR,,,,,synthetic-sell-3,,,,)";
    constexpr std::string_view januaryPurchase =
        R"(2025-01-05T09:00:00Z,2025-01-05,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,4,16,-64,,,EUR,,,,,synthetic-buy-3,,,,)";
    constexpr std::string_view februarySale =
        R"(2025-02-10T09:00:00Z,2025-02-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,22,44,,,EUR,,,,,synthetic-sell-4,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };

    const TemporaryHistoryInputs files;
    const auto olderPath =
        files.write("history_2008_2023.csv", {purchase2008, sale2023, purchase2023});
    const auto newerPath =
        files.write("history_2024_2025.csv",
                    {split2024, juneSale, decemberSale, januaryPurchase, februarySale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(olderPath)},
                                            {1, parser.parse(newerPath, 1)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher::match(prepared.mMerged.mStatement,
                                         *prepared.mLedger,
                                         {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                                           .mRatio = {.mNewShares = 2, .mOldShares = 1},
                                           .mEffectiveDate = *parseCalendarDate("2024-02-01")}});
    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(prepared.mMerged.mStatement,
                                                    *prepared.mLedger,
                                                    fifo,
                                                    decisions);

    EXPECT_TRUE(fifo.mDiagnostics.empty());
    EXPECT_TRUE(loss.mRequests.empty()); // the January 2025 purchase decides the December loss
    EXPECT_TRUE(loss.mDiagnostics.empty());

    const auto built = CapitalGainsReportBuilder::build(prepared.mMerged.mStatement,
                                                        *prepared.mLedger,
                                                        fifo,
                                                        loss,
                                                        {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
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
              <F1>2023-09-01</F1>
              <F2>B</F2>
              <F3>12.0000</F3>
              <F4>20.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>12.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-06-20</F6>
              <F7>8.0000</F7>
              <F9>25.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>4.0000</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Sale>
              <F6>2024-12-20</F6>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 12, 12 - 8 = 4, 4 - 4 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"12", "4", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"12", "4", "0"}));
    // 8 x (25 - 20) = 40 and 4 x (15 - 20) = -20.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"40", "-20"}));

    ASSERT_EQ(built.mLongHeld.size(), 1U);
    // The June sale (the fourth trade) uses the 12 units of 2008, after the split.
    EXPECT_EQ(built.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 3},
                  .mUnits = 1'200'000'000,
              }));
}

TEST(HistoricalScenarioIntegrationTest, TheExampleFromTwoFilesWritesItsRows2025) {
    // The same files for 2025. After the 2024 sales 4 units of the 2023 lot (EUR 20 a unit after
    // the split) are left, then the 4 units bought on 2025-01-05. The sale of 2 units at EUR 22 on
    // 2025-02-10 takes 2 units of the 2023 lot: 2 x (22 - 20) = EUR 4, a profit, so F10 true. The
    // 2025-01-05 purchase is not used by a sale, so it has no row, and the 2024 sales are not
    // listed.
    constexpr std::string_view purchase2008 =
        R"(2008-05-10T09:00:00Z,2008-05-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,5,-50,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sale2023 =
        R"(2023-03-01T09:00:00Z,2023-03-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,6,24,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view purchase2023 =
        R"(2023-09-01T09:00:00Z,2023-09-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,8,40,-320,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view split2024 =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,12,,,,,,,,,,synthetic-split-1,,,,)";
    constexpr std::string_view juneSale =
        R"(2024-06-20T09:00:00Z,2024-06-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-20,25,500,,,EUR,,,,,synthetic-sell-2,,,,)";
    constexpr std::string_view decemberSale =
        R"(2024-12-20T09:00:00Z,2024-12-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,15,60,,,EUR,,,,,synthetic-sell-3,,,,)";
    constexpr std::string_view januaryPurchase =
        R"(2025-01-05T09:00:00Z,2025-01-05,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,4,16,-64,,,EUR,,,,,synthetic-buy-3,,,,)";
    constexpr std::string_view februarySale =
        R"(2025-02-10T09:00:00Z,2025-02-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,22,44,,,EUR,,,,,synthetic-sell-4,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2025,
        .mFilingType = FilingType::Original,
    };

    const TemporaryHistoryInputs files;
    const auto olderPath =
        files.write("history_2008_2023.csv", {purchase2008, sale2023, purchase2023});
    const auto newerPath =
        files.write("history_2024_2025.csv",
                    {split2024, juneSale, decemberSale, januaryPurchase, februarySale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(olderPath)},
                                            {1, parser.parse(newerPath, 1)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2025, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher::match(prepared.mMerged.mStatement,
                                         *prepared.mLedger,
                                         {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                                           .mRatio = {.mNewShares = 2, .mOldShares = 1},
                                           .mEffectiveDate = *parseCalendarDate("2024-02-01")}});
    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(prepared.mMerged.mStatement,
                                                    *prepared.mLedger,
                                                    fifo,
                                                    decisions);

    EXPECT_TRUE(fifo.mDiagnostics.empty());
    EXPECT_TRUE(loss.mRequests.empty()); // the only 2025 sale is a gain
    EXPECT_TRUE(loss.mDiagnostics.empty());

    const auto built = CapitalGainsReportBuilder::build(prepared.mMerged.mStatement,
                                                        *prepared.mLedger,
                                                        fifo,
                                                        loss,
                                                        {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
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
              <F1>2023-09-01</F1>
              <F2>B</F2>
              <F3>2.0000</F3>
              <F4>20.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>2.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2025-02-10</F6>
              <F7>2.0000</F7>
              <F9>22.0000</F9>
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

    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 2, 2 - 2 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"2", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"2", "0"}));
    // 2 x (22 - 20) = 4.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"4"}));

    EXPECT_TRUE(built.mLongHeld.empty());
}

TEST(HistoricalScenarioIntegrationTest, OneFileWithTheWholeHistoryWritesTheSameRows2024) {
    // The eight rows of the example in one file give the same Doh-KDVP rows for 2024 as the two
    // files above (tax_rules.md, "Files covering several years").
    constexpr std::string_view purchase2008 =
        R"(2008-05-10T09:00:00Z,2008-05-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,5,-50,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sale2023 =
        R"(2023-03-01T09:00:00Z,2023-03-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,6,24,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view purchase2023 =
        R"(2023-09-01T09:00:00Z,2023-09-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,8,40,-320,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view split2024 =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,12,,,,,,,,,,synthetic-split-1,,,,)";
    constexpr std::string_view juneSale =
        R"(2024-06-20T09:00:00Z,2024-06-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-20,25,500,,,EUR,,,,,synthetic-sell-2,,,,)";
    constexpr std::string_view decemberSale =
        R"(2024-12-20T09:00:00Z,2024-12-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,15,60,,,EUR,,,,,synthetic-sell-3,,,,)";
    constexpr std::string_view januaryPurchase =
        R"(2025-01-05T09:00:00Z,2025-01-05,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,4,16,-64,,,EUR,,,,,synthetic-buy-3,,,,)";
    constexpr std::string_view februarySale =
        R"(2025-02-10T09:00:00Z,2025-02-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,22,44,,,EUR,,,,,synthetic-sell-4,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };

    const TemporaryHistoryInputs files;
    const auto historyPath = files.write("history_2008_2025.csv",
                                         {purchase2008,
                                          sale2023,
                                          purchase2023,
                                          split2024,
                                          juneSale,
                                          decemberSale,
                                          januaryPurchase,
                                          februarySale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(historyPath)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher::match(prepared.mMerged.mStatement,
                                         *prepared.mLedger,
                                         {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                                           .mRatio = {.mNewShares = 2, .mOldShares = 1},
                                           .mEffectiveDate = *parseCalendarDate("2024-02-01")}});
    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(prepared.mMerged.mStatement,
                                                    *prepared.mLedger,
                                                    fifo,
                                                    decisions);

    EXPECT_TRUE(fifo.mDiagnostics.empty());
    EXPECT_TRUE(loss.mRequests.empty()); // the January 2025 purchase decides the December loss
    EXPECT_TRUE(loss.mDiagnostics.empty());

    const auto built = CapitalGainsReportBuilder::build(prepared.mMerged.mStatement,
                                                        *prepared.mLedger,
                                                        fifo,
                                                        loss,
                                                        {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
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
              <F1>2023-09-01</F1>
              <F2>B</F2>
              <F3>12.0000</F3>
              <F4>20.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>12.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-06-20</F6>
              <F7>8.0000</F7>
              <F9>25.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>4.0000</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Sale>
              <F6>2024-12-20</F6>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 12, 12 - 8 = 4, 4 - 4 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"12", "4", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"12", "4", "0"}));
    // 8 x (25 - 20) = 40 and 4 x (15 - 20) = -20.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"40", "-20"}));

    ASSERT_EQ(built.mLongHeld.size(), 1U);
    // The June sale (the fourth trade) uses the 12 units of 2008, after the split.
    EXPECT_EQ(built.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 3},
                  .mUnits = 1'200'000'000,
              }));
}

TEST(HistoricalScenarioIntegrationTest, TheExampleFromTwoFilesInReverseOrderWritesItsRows2024) {
    // The files of the first test, but the newer file is input 0 and the older one input 1
    // (tax_rules.md, "Files covering several years": the order in which files arrive has no tax
    // meaning). The tax dates decide the order, so the rows and amounts are the same as there: the
    // split 2 / 1, the June sale of 20 gains 8 x (25 - 20) = EUR 40, the December sale of 4 loses
    // 4 x (15 - 20) = EUR 20 and the purchase of 4 units on 2025-01-05 blocks it: F10 false.
    constexpr std::string_view purchase2008 =
        R"(2008-05-10T09:00:00Z,2008-05-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,5,-50,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sale2023 =
        R"(2023-03-01T09:00:00Z,2023-03-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,6,24,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view purchase2023 =
        R"(2023-09-01T09:00:00Z,2023-09-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,8,40,-320,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view split2024 =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,12,,,,,,,,,,synthetic-split-1,,,,)";
    constexpr std::string_view juneSale =
        R"(2024-06-20T09:00:00Z,2024-06-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-20,25,500,,,EUR,,,,,synthetic-sell-2,,,,)";
    constexpr std::string_view decemberSale =
        R"(2024-12-20T09:00:00Z,2024-12-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,15,60,,,EUR,,,,,synthetic-sell-3,,,,)";
    constexpr std::string_view januaryPurchase =
        R"(2025-01-05T09:00:00Z,2025-01-05,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,4,16,-64,,,EUR,,,,,synthetic-buy-3,,,,)";
    constexpr std::string_view februarySale =
        R"(2025-02-10T09:00:00Z,2025-02-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,22,44,,,EUR,,,,,synthetic-sell-4,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };

    const TemporaryHistoryInputs files;
    const auto olderPath =
        files.write("history_2008_2023.csv", {purchase2008, sale2023, purchase2023});
    const auto newerPath =
        files.write("history_2024_2025.csv",
                    {split2024, juneSale, decemberSale, januaryPurchase, februarySale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(newerPath)},
                                            {1, parser.parse(olderPath, 1)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher::match(prepared.mMerged.mStatement,
                                         *prepared.mLedger,
                                         {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                                           .mRatio = {.mNewShares = 2, .mOldShares = 1},
                                           .mEffectiveDate = *parseCalendarDate("2024-02-01")}});
    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(prepared.mMerged.mStatement,
                                                    *prepared.mLedger,
                                                    fifo,
                                                    decisions);

    EXPECT_TRUE(fifo.mDiagnostics.empty());
    EXPECT_TRUE(loss.mRequests.empty()); // the January 2025 purchase decides the December loss
    EXPECT_TRUE(loss.mDiagnostics.empty());

    const auto built = CapitalGainsReportBuilder::build(prepared.mMerged.mStatement,
                                                        *prepared.mLedger,
                                                        fifo,
                                                        loss,
                                                        {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
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
              <F1>2023-09-01</F1>
              <F2>B</F2>
              <F3>12.0000</F3>
              <F4>20.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>12.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-06-20</F6>
              <F7>8.0000</F7>
              <F9>25.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>4.0000</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Sale>
              <F6>2024-12-20</F6>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 12, 12 - 8 = 4, 4 - 4 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"12", "4", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"12", "4", "0"}));
    // 8 x (25 - 20) = 40 and 4 x (15 - 20) = -20.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"40", "-20"}));

    ASSERT_EQ(built.mLongHeld.size(), 1U);
    // The June sale (the fourth trade) uses the 12 units of 2008, after the split.
    EXPECT_EQ(built.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 3},
                  .mUnits = 1'200'000'000,
              }));
}

TEST(HistoricalScenarioIntegrationTest, OverlappingFilesWriteTheSameRows2024) {
    // The first file holds the rows up to the December sale; the second starts at the split, so
    // the split and both 2024 sales are in both files with the same transaction IDs. They merge
    // into one event each, and the rows and amounts are the same as in the first test.
    constexpr std::string_view purchase2008 =
        R"(2008-05-10T09:00:00Z,2008-05-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,5,-50,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sale2023 =
        R"(2023-03-01T09:00:00Z,2023-03-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,6,24,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view purchase2023 =
        R"(2023-09-01T09:00:00Z,2023-09-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,8,40,-320,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view split2024 =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,12,,,,,,,,,,synthetic-split-1,,,,)";
    constexpr std::string_view juneSale =
        R"(2024-06-20T09:00:00Z,2024-06-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-20,25,500,,,EUR,,,,,synthetic-sell-2,,,,)";
    constexpr std::string_view decemberSale =
        R"(2024-12-20T09:00:00Z,2024-12-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,15,60,,,EUR,,,,,synthetic-sell-3,,,,)";
    constexpr std::string_view januaryPurchase =
        R"(2025-01-05T09:00:00Z,2025-01-05,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,4,16,-64,,,EUR,,,,,synthetic-buy-3,,,,)";
    constexpr std::string_view februarySale =
        R"(2025-02-10T09:00:00Z,2025-02-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,22,44,,,EUR,,,,,synthetic-sell-4,,,,)";
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };

    const TemporaryHistoryInputs files;
    const auto olderPath =
        files.write("history_2008_2024.csv",
                    {purchase2008, sale2023, purchase2023, split2024, juneSale, decemberSale});
    const auto newerPath =
        files.write("history_2024_2025.csv",
                    {split2024, juneSale, decemberSale, januaryPurchase, februarySale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(olderPath)},
                                            {1, parser.parse(newerPath, 1)}};

    const auto prepared = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher::match(prepared.mMerged.mStatement,
                                         *prepared.mLedger,
                                         {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                                           .mRatio = {.mNewShares = 2, .mOldShares = 1},
                                           .mEffectiveDate = *parseCalendarDate("2024-02-01")}});
    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(prepared.mMerged.mStatement,
                                                    *prepared.mLedger,
                                                    fifo,
                                                    decisions);

    EXPECT_TRUE(fifo.mDiagnostics.empty());
    EXPECT_TRUE(loss.mRequests.empty()); // the January 2025 purchase decides the December loss
    EXPECT_TRUE(loss.mDiagnostics.empty());

    const auto built = CapitalGainsReportBuilder::build(prepared.mMerged.mStatement,
                                                        *prepared.mLedger,
                                                        fifo,
                                                        loss,
                                                        {});

    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
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
              <F1>2023-09-01</F1>
              <F2>B</F2>
              <F3>12.0000</F3>
              <F4>20.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>12.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-06-20</F6>
              <F7>8.0000</F7>
              <F9>25.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>4.0000</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Sale>
              <F6>2024-12-20</F6>
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

    const auto fifoLists = fifoOverKdvpRows(*result.mXml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 12, 12 - 8 = 4, 4 - 4 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"12", "4", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"12", "4", "0"}));
    // 8 x (25 - 20) = 40 and 4 x (15 - 20) = -20.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"40", "-20"}));

    ASSERT_EQ(built.mLongHeld.size(), 1U);
    // The June sale (the fourth trade) uses the 12 units of 2008, after the split.
    EXPECT_EQ(built.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 3},
                  .mUnits = 1'200'000'000,
              }));
}

} // namespace
