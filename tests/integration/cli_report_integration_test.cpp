#include "../support/kdvp_row_fifo.hpp"
#include "../support/xsd_validation.hpp"
#include "cli/report_command.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
using taxbroker::cli::kExitBlocked;
using taxbroker::cli::kExitFailure;
using taxbroker::cli::kExitSuccess;
using taxbroker::cli::runCli;
using taxbroker::test::expectValidForKdvpSchemas;
using taxbroker::test::fifoOverKdvpRows;

struct CliRun {
    int mExitCode{};
    std::string mOut;
    std::string mErr;
};

/// A temporary folder for the input files of one test, with `out` as the output folder.
class TemporaryCliFolder {
  public:
    TemporaryCliFolder() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_cli_synthetic_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create the synthetic CLI directory"};
        }
    }

    TemporaryCliFolder(const TemporaryCliFolder&) = delete;
    TemporaryCliFolder& operator=(const TemporaryCliFolder&) = delete;

    ~TemporaryCliFolder() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    /// Writes the Trade Republic header and exactly the rows supplied by the test.
    [[nodiscard]] std::string writeCsv(std::string_view aName,
                                       std::initializer_list<std::string_view> aRows) const {
        std::string text =
            "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,"
            "amount,fee,tax,currency,original_amount,original_currency,fx_rate,description,"
            "transaction_id,counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

        for (const auto row : aRows)
        {
            text += std::string{row} + '\n';
        }

        return writeText(aName, text);
    }

    [[nodiscard]] std::string writeText(std::string_view aName, std::string_view aText) const {
        const auto path = mDirectory / aName;
        std::ofstream output{path, std::ios::binary};

        output << aText;
        output.close();

        if (!output)
        {
            throw std::runtime_error{"Unable to write a synthetic CLI file"};
        }

        return path.string();
    }

    [[nodiscard]] std::string outputDirectory() const {
        return (mDirectory / "out").string();
    }

    [[nodiscard]] bool hasOutput(std::string_view aName) const {
        return std::filesystem::exists(mDirectory / "out" / aName);
    }

    [[nodiscard]] std::string readOutput(std::string_view aName) const {
        std::ifstream input{mDirectory / "out" / aName, std::ios::binary};

        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

  private:
    std::filesystem::path mDirectory;
};

CliRun run(const std::vector<std::string>& aArguments) {
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = runCli(aArguments, out, err);

    return CliRun{exitCode, out.str(), err.str()};
}

bool contains(const std::string& aText, std::string_view aPart) {
    return aText.find(aPart) != std::string::npos;
}

constexpr std::string_view kTaxpayer = R"({"taxNumber": "12345678", "isResident": true})";

TEST(CliReportIntegrationTest, TwoFilesWriteTheGoldenXmlAndSummary2024) {
    // Example "one sale uses two purchases" in tax_rules.md, with the purchases and the sale in
    // two files.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto purchases = folder.writeCsv(
        "purchases.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,20,20,-400,,,EUR,,,,,synthetic-buy-2,,,,)",
        });
    const auto sales = folder.writeCsv(
        "sales.csv",
        {
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,30,450,,,EUR,,,,,synthetic-sell,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             purchases,
                             sales});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());

    const std::string expectedSummary = R"(TaxBrokerReport: Doh-KDVP 2024 (original return)
Input files: 2
  source-0001 purchases.csv
  source-0002 sales.csv

Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2024.xml

Investments with a sale in 2024: 1
  XX0000000001 Synthetic Share: listed, 1 sale, 15.0000 units

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 0

Warnings: 0
)";

    EXPECT_EQ(result.mOut, expectedSummary);
    EXPECT_EQ(folder.readOutput("summary_2024.txt"), expectedSummary);
    // Both files were written under a temporary name first and then renamed.
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml.tmp"));
    EXPECT_FALSE(folder.hasOutput("summary_2024.txt.tmp"));

    const auto xml = folder.readOutput("Doh-KDVP_2024.xml");

    // Row 1 lists only the 5 units of the 20 @ 20 purchase that the sale uses. F8 is the running
    // stock of the listed rows: 10, 10 + 5 = 15, 15 - 15 = 0.
    EXPECT_EQ(xml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
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

    expectValidForKdvpSchemas(xml);

    const auto fifoLists = fifoOverKdvpRows(xml);

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

TEST(CliReportIntegrationTest, SelfReportWritesWorkflowCodeI) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--self-report",
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut, "TaxBrokerReport: Doh-KDVP 2024 (self-report)\n"));

    const auto xml = folder.readOutput("Doh-KDVP_2024.xml");

    EXPECT_TRUE(contains(xml, "<edp:DocumentWorkflowID>I</edp:DocumentWorkflowID>"));
    EXPECT_TRUE(contains(xml, "<edp:DocumentWorkflowName>SamoPrijava</edp:DocumentWorkflowName>"));
    EXPECT_TRUE(contains(xml, "<DocumentWorkflowID>I</DocumentWorkflowID>"));
    EXPECT_TRUE(contains(xml, "<DocumentWorkflowName>SamoPrijava</DocumentWorkflowName>"));
    EXPECT_FALSE(contains(xml, ">O<"));

    expectValidForKdvpSchemas(xml);
}

TEST(CliReportIntegrationTest, SaleOfOldAndNewUnitsListsOnlyTheNewUnitAndSummarisesTheOld2025) {
    // Example in "Long-held investments" in tax_rules.md (sale in 2025, so 15 years). FIFO takes
    // the 2 units from 2005, which are past the 15 years, and 1 unit from 2020.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2005-04-01T09:00:00Z,2005-04-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2020-04-01T09:00:00Z,2020-04-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,20,-40,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2025-06-02T09:00:00Z,2025-06-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,30,90,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    // The listed unit is the one from 2020; the 2 units from 2005 are in the summary only.
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2025 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2025.xml

Investments with a sale in 2025: 1
  XX0000000001 Synthetic Share: listed, 1 sale, 1.0000 units

Units past the holding period (tax free, not in the XML): 1
  XX0000000001 Synthetic Share: sale 2025-06-02, 2.0000 units

Private-market events skipped (only a cash amount, no units): 0

Errors: 0

Warnings: 0
)");

    const auto xml = folder.readOutput("Doh-KDVP_2025.xml");

    // Two rows: purchase 2020-04-01 with F3 1, sale 2025-06-02 with F7 1. The 2005 purchase has
    // no row.
    EXPECT_EQ(xml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
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

    expectValidForKdvpSchemas(xml);

    const auto fifoLists = fifoOverKdvpRows(xml);

    ASSERT_EQ(fifoLists.size(), 1U);
    // Only the listed unit: 1 x (30 - 20) = 10.
    EXPECT_EQ(fifoLists.at(0).mSaleGains, (std::vector<std::string>{"10"}));
}

TEST(CliReportIntegrationTest, NoSaleInTheYearGivesNoDataAndNoXml) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2025 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): no data, no sale in 2025 needs a Doh-KDVP form

Investments with a sale in 2025: 0

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 0

Warnings: 0
)");
    EXPECT_EQ(folder.readOutput("summary_2025.txt"), result.mOut);
}

TEST(CliReportIntegrationTest, PrivateMarketEventsAreSkippedWithAWarning) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-05-02T09:00:00Z,2024-05-02,DEFAULT,CASH,PRIVATE_MARKET_BUY,PRIVATE_FUND,Synthetic Private Fund,XX0000000009,,,-100,,,EUR,,,,,synthetic-private-buy-1,,,,)",
            R"(2024-11-15T09:00:00Z,2024-11-15,DEFAULT,CASH,PRIVATE_MARKET_SELL,PRIVATE_FUND,Synthetic Private Fund,XX0000000009,,,150,,,EUR,,,,,synthetic-private-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // The private-market sale does not block: the share is reported, the fund is skipped.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_FALSE(contains(folder.readOutput("Doh-KDVP_2024.xml"), "XX0000000009"));
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2024 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2024.xml

Investments with a sale in 2024: 1
  XX0000000001 Synthetic Share: listed, 1 sale, 10.0000 units

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 2
  XX0000000009 Synthetic Private Fund: 2024-05-02, history.csv row 4
  XX0000000009 Synthetic Private Fund: 2024-11-15, history.csv row 5

Errors: 0

Warnings: 2
  diagnostic-0001 unsupported_tax_treatment, XX0000000009 Synthetic Private Fund, 2024-05-02, history.csv row 4
    Private-market events have only a cash amount, without units or a price, so they are skipped and are not in the Doh-KDVP report. Check whether you have to report them yourself.
  diagnostic-0002 unsupported_tax_treatment, XX0000000009 Synthetic Private Fund, 2024-11-15, history.csv row 5
    Private-market events have only a cash amount, without units or a price, so they are skipped and are not in the Doh-KDVP report. Check whether you have to report them yourself.
)");
}

TEST(CliReportIntegrationTest, PrivateMarketSaleWithoutAnIsinIsListedByItsName) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
            // symbol is blank.
            R"(2024-05-02T09:00:00Z,2024-05-02,DEFAULT,CASH,PRIVATE_MARKET_SELL,PRIVATE_FUND,Synthetic Private Fund,,,,150,,,EUR,,,,,synthetic-private-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Private-market events skipped (only a cash amount, no units): 1\n"
                         "  (no ISIN) Synthetic Private Fund: 2024-05-02, history.csv row 4\n"));
    // Only the name, with one space after the comma.
    EXPECT_TRUE(contains(result.mOut,
                         "Warnings: 1\n"
                         "  diagnostic-0001 unsupported_tax_treatment, Synthetic Private Fund, "
                         "2024-05-02, history.csv row 4\n"));
}

TEST(CliReportIntegrationTest, WarningOnlyRunStillGeneratesTheXml) {
    // A crypto purchase without a sale keeps only its parser warning.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,CRYPTO,Synthetic Coin,XX0000000005,1,50,-50,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2024.xml\n"));
    // A parser diagnostic names the file and row, but no investment or tax date.
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 0\n\nWarnings: 1\n"
                         "  diagnostic-0001 unsupported_asset_class, history.csv row 3\n"
                         "    Asset class 'CRYPTO' was preserved, but its tax treatment is not "
                         "supported yet.\n"));
}

TEST(CliReportIntegrationTest, CryptoSaleBlocksTheReport) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,CRYPTO,Synthetic Coin,XX0000000005,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,CRYPTO,Synthetic Coin,XX0000000005,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000005 Synthetic Coin: blocked (unsupported_tax_treatment)\n"
                         "    sale 2024-03-10, history.csv row 3\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0003 unsupported_tax_treatment, XX0000000005 Synthetic "
                         "Coin, 2024-03-10, history.csv row 3\n"
                         "    Sales of the asset class crypto are not supported in the "
                         "capital-gains report yet, so this investment's sales cannot be "
                         "reported. No Doh-KDVP file was written.\n"));
}

TEST(CliReportIntegrationTest, PrivateFundTradeSaleBlocksTheReport) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,PRIVATE_FUND,Synthetic Private Fund,XX0000000009,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,PRIVATE_FUND,Synthetic Private Fund,XX0000000009,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000009 Synthetic Private Fund: blocked "
                         "(unsupported_tax_treatment)\n"
                         "    sale 2024-03-10, history.csv row 3\n"));
    // The parser's two class warnings come first, so the gate's error is the third diagnostic.
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0003 unsupported_tax_treatment, XX0000000009 Synthetic "
                         "Private Fund, 2024-03-10, history.csv row 3\n"
                         "    Sales of the asset class private_fund are not supported in the "
                         "capital-gains report yet, so this investment's sales cannot be "
                         "reported. No Doh-KDVP file was written.\n"));
}

TEST(CliReportIntegrationTest, BlankAssetClassBlocksTheReport) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,,Synthetic Share,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked (tax_classification_unknown)\n"
                         "    sale 2024-03-10, history.csv row 3\n"));
    // The parser's errors for the two rows come first, then the gate's.
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 3\n"
                         "  diagnostic-0001 missing_field, history.csv row 2\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  diagnostic-0003 tax_classification_unknown, XX0000000001 Synthetic "
                         "Share, 2024-03-10, history.csv row 3\n"));
}

TEST(CliReportIntegrationTest, ConflictingClassAcrossFilesBlocksTheReport) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto purchases = folder.writeCsv(
        "purchases.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
        });
    const auto sales = folder.writeCsv(
        "sales.csv",
        {
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,FUND,Synthetic Share,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             purchases,
                             sales});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    // STOCK in one file and FUND in the other: the merger reports the conflict, the gate the
    // unknown class.
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked "
                         "(instrument_asset_class_conflict, tax_classification_unknown)\n"
                         "    sale 2024-03-10, sales.csv row 2\n"));
    // The merger's conflict names both rows; the gate's error names the sale.
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 2\n"
                         "  diagnostic-0001 instrument_asset_class_conflict, XX0000000001 "
                         "Synthetic Share, purchases.csv row 2 (+1 more)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  diagnostic-0002 tax_classification_unknown, XX0000000001 Synthetic "
                         "Share, 2024-03-10, sales.csv row 2\n"));
}

TEST(CliReportIntegrationTest, SplitBeforeTheSaleBlocksTheReport) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2022-01-10T09:00:00Z,2022-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2023-01-15T09:00:00Z,2023-01-15,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,1,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,30,60,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked (corporate_action_not_applied)\n"
                         "    sale 2025-03-10, history.csv row 4\n"));
    // The error points at the split, not at the sale.
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 corporate_action_not_applied, XX0000000001 Synthetic "
                         "Share, 2023-01-15, history.csv row 3\n"));
}

TEST(CliReportIntegrationTest, SplitAfterTheSaleInTheSameYearBlocksTheReport) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-09-01T09:00:00Z,2025-09-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,1,,,,,,,,,,synthetic-split-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // Any corporate action up to 31 December blocks, also one after the sale.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked (corporate_action_not_applied)\n"
                         "    sale 2025-03-10, history.csv row 3\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  diagnostic-0001 corporate_action_not_applied, XX0000000001 Synthetic "
                         "Share, 2025-09-01, history.csv row 4\n"));
}

TEST(CliReportIntegrationTest, SaleWithoutEnoughPurchasesBlocksTheReport) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,30,60,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // 2 units sold, 1 bought.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked (incomplete_history)\n"
                         "    sale 2025-03-10, history.csv row 3\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 incomplete_history, XX0000000001 Synthetic Share, "
                         "2025-03-10, history.csv row 3\n"));
}

TEST(CliReportIntegrationTest, ALossTheFilesCannotDecideIsWrittenAsBlockedWithAWarning) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // 10 x 20 - 10 x 30 = -100: a loss, and no purchase within 30 days blocks it. The loss rule
    // needs the answer whether the investment was also traded elsewhere; this command cannot ask
    // yet, so all 10 loss units are assumed blocked (F10 false), with one warning.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2025 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2025.xml

Investments with a sale in 2025: 1
  XX0000000001 Synthetic Share: listed, 1 sale, 10.0000 units, 10.0000 units with a loss blocked by the 30-day rule (F10 false), of them 10.0000 assumed without an answer (see the warnings below)

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 0

Warnings: 1
  diagnostic-0001 loss_eligibility_unresolved, XX0000000001 Synthetic Share, 2025-03-10, history.csv row 3
    A sale of this investment was made at a loss, and your files alone cannot decide whether the 30-day rule blocks the loss. This command cannot ask you yet, so the whole open loss was treated as not deductible (F10 false). If, within 30 days before or after the sale, you did not buy or sell this investment anywhere that is not in your files, correct it yourself in eDavki.
)");
    EXPECT_EQ(folder.readOutput("summary_2025.txt"), result.mOut);

    const auto xml = folder.readOutput("Doh-KDVP_2025.xml");

    EXPECT_TRUE(contains(xml, "<F10>false</F10>"));
    expectValidForKdvpSchemas(xml);
}

TEST(CliReportIntegrationTest, ProfitableSaleWithALossMakingPortionBlocksTheReport) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-02-10T09:00:00Z,2025-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,40,-40,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,30,60,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // Portions: 30 - 10 = +20 and 30 - 40 = -10. The sale gains 10 overall, but its loss unit
    // is assumed blocked (no answer) and its profitable unit is not: such a sale needs split rows,
    // so it blocks the report.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked (unsupported_tax_treatment)\n"
                         "    sale 2025-03-10, history.csv row 4\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 unsupported_tax_treatment, XX0000000001 Synthetic "
                         "Share, 2025-03-10, history.csv row 4\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Warnings: 1\n"
                         "  diagnostic-0002 loss_eligibility_unresolved, XX0000000001 Synthetic "
                         "Share, 2025-03-10, history.csv row 4\n"));
}

TEST(CliReportIntegrationTest, ALossBlockedByAJanuaryPurchaseIsWritten) {
    // tax_rules.md: buy 100 in September 2023, sell 4 at a loss on 2024-12-20, buy 4 on
    // 2025-01-05. The January purchase blocks the whole loss, so the command needs no answer.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2023-09-01T09:00:00Z,2023-09-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,10,-1000,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-12-20T09:00:00Z,2024-12-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,8,32,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-01-05T09:00:00Z,2025-01-05,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,4,8,-32,,,EUR,,,,,synthetic-buy-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: listed, 1 sale, 4.0000 units, 4.0000 "
                         "units with a loss blocked by the 30-day rule (F10 false)\n"));
    EXPECT_TRUE(contains(result.mOut, "Errors: 0\n"));

    const auto xml = folder.readOutput("Doh-KDVP_2024.xml");

    EXPECT_TRUE(contains(xml, "<F10>false</F10>"));
    EXPECT_FALSE(contains(xml, "<F1>2025-01-05</F1>"));
    expectValidForKdvpSchemas(xml);
}

TEST(CliReportIntegrationTest, NonEurTradeBlocksTheReport) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,USD,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked (missing_exchange_rate)\n"
                         "    sale 2025-03-10, history.csv row 3\n"));
    // The error points at the USD purchase, where FIFO stopped.
    EXPECT_TRUE(contains(result.mOut,
                         "  diagnostic-0001 missing_exchange_rate, XX0000000001 Synthetic Share, "
                         "2025-01-10, history.csv row 2\n"
                         "    A trade of this investment is in USD, not in EUR."));
}

TEST(CliReportIntegrationTest, RejectedTradeRowBlocksTheReport) {
    // The share alone would pass; the rejected row of another investment blocks the whole report.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            // shares is "one", not a number.
            R"(2025-02-10T09:00:00Z,2025-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Other Share,XX0000000002,one,10,-10,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: no problem, 1 sale, 1.0000 units\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 invalid_value, history.csv row 3\n"));
}

TEST(CliReportIntegrationTest, ConflictingDuplicateBlocksTheReport) {
    // The same transaction ID with another price in the second file. The merger rejects both
    // copies, so FIFO never sees them; the share alone would pass.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto first = folder.writeCsv(
        "first.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-02-10T09:00:00Z,2025-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Other Share,XX0000000002,1,10,-10,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const auto second = folder.writeCsv(
        "second.csv",
        {
            R"(2025-02-10T09:00:00Z,2025-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Other Share,XX0000000002,1,11,-11,,,EUR,,,,,synthetic-buy-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             first,
                             second});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: no problem, 1 sale, 1.0000 units\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 conflicting_duplicate, XX0000000002 Synthetic Other "
                         "Share, 2025-02-10, first.csv row 3 (+1 more)\n"));
}

TEST(CliReportIntegrationTest, RejectedPrivateMarketRowIsListedApartAndDoesNotBlock) {
    // Private-market events are skipped, so a rejected one changes no form: the share is still
    // reported, and the error is listed apart.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
            // amount is "ten", not a number.
            R"(2024-06-02T09:00:00Z,2024-06-02,DEFAULT,CASH,PRIVATE_MARKET_BUY,PRIVATE_FUND,Synthetic Private Fund,XX0000000009,,,ten,,,EUR,,,,,synthetic-private-buy-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_FALSE(contains(folder.readOutput("Doh-KDVP_2024.xml"), "XX0000000009"));
    // The rejected row is not an event, so nothing is listed as skipped.
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2024 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2024.xml

Investments with a sale in 2024: 1
  XX0000000001 Synthetic Share: listed, 1 sale, 10.0000 units

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 0

Warnings: 0

Not about Doh-KDVP (dividends, interest and skipped private-market rows): 1
  diagnostic-0001 error invalid_value, history.csv row 4
    Field 'amount' has an invalid value in private-market row; the row was skipped.
)");
}

TEST(CliReportIntegrationTest, TaxpayerValueTheFormDoesNotAcceptBlocksTheReport) {
    // postCode has 13 characters; the form's postNumber allows 12.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText(
        "taxpayer.json",
        R"({"taxNumber": "12345678", "isResident": true, "postCode": "1234567890123"})");
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    // The error names the field, never the value.
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 xml_value_out_of_range\n"
                         "    The value of the XML field postNumber is outside what the FURS form "
                         "allows"));
    EXPECT_FALSE(contains(result.mOut, "1234567890123"));
}

TEST(CliReportIntegrationTest, BlockedRunDeletesAnOldXmlOfTheSameYear) {
    // The purchase is in USD, so the FIFO pool stops and the report is blocked.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,USD,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    std::filesystem::create_directory(folder.outputDirectory());
    (void)folder.writeText("out/Doh-KDVP_2025.xml", "an old file of an earlier run");

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"
                         "Deleted the old Doh-KDVP_2025.xml of an earlier run.\n"));
}

TEST(CliReportIntegrationTest, YearWithoutDataDeletesAnOldXmlOfTheSameYear) {
    // Only a purchase in 2025, so no sale needs a form.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
        });
    std::filesystem::create_directory(folder.outputDirectory());
    (void)folder.writeText("out/Doh-KDVP_2025.xml", "an old file of an earlier run");

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): no data, no sale in 2025 needs a Doh-KDVP "
                         "form\n"
                         "Deleted the old Doh-KDVP_2025.xml of an earlier run.\n"));
}

TEST(CliReportIntegrationTest, GeneratedRunReplacesTheFilesOfAnEarlierRunAndKeepsOtherYears) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    std::filesystem::create_directory(folder.outputDirectory());
    (void)folder.writeText("out/Doh-KDVP_2024.xml", "an old file of an earlier run");
    (void)folder.writeText("out/summary_2024.txt", "an old summary of an earlier run");
    (void)folder.writeText("out/Doh-KDVP_2023.xml", "the file of another year");
    (void)folder.writeText("out/summary_2023.txt", "the summary of another year");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    // Replacing a file of the same year is not a deletion, so the summary does not mention it.
    EXPECT_FALSE(contains(result.mOut, "Deleted the old"));

    // The files of 2024 are this run's.
    EXPECT_TRUE(contains(folder.readOutput("Doh-KDVP_2024.xml"), "<F7>10.0000</F7>"));
    EXPECT_EQ(folder.readOutput("summary_2024.txt"), result.mOut);

    // The files of 2023 are not touched.
    EXPECT_EQ(folder.readOutput("Doh-KDVP_2023.xml"), "the file of another year");
    EXPECT_EQ(folder.readOutput("summary_2023.txt"), "the summary of another year");
}

TEST(CliReportIntegrationTest, TransactionInTwoOverlappingFilesIsCountedOnce) {
    // The sale is in both files with the same transaction ID and the same values.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto first = folder.writeCsv(
        "first.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const auto second = folder.writeCsv(
        "second.csv",
        {
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             first,
                             second});

    // Counted twice, 20 units would be sold with 10 bought, and the report would be blocked.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2024 (original return)
Input files: 2
  source-0001 first.csv
  source-0002 second.csv

Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2024.xml

Investments with a sale in 2024: 1
  XX0000000001 Synthetic Share: listed, 1 sale, 10.0000 units

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 0

Warnings: 0
)");

    const auto xml = folder.readOutput("Doh-KDVP_2024.xml");

    EXPECT_TRUE(contains(xml, "<F7>10.0000</F7>"));
    expectValidForKdvpSchemas(xml);
}

TEST(CliReportIntegrationTest, FileThatIsNotATradeRepublicExportBlocksTheReport) {
    // The share in the first file alone would pass.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    // 3 of the 23 columns: 15 required columns are missing.
    const auto other = folder.writeText("other.csv", "date,type,shares\n2024-01-10,BUY,1\n");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history,
                             other});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    // The error names only the file, without a row or an investment.
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2024 (original return)
Input files: 2
  source-0001 history.csv
  source-0002 other.csv

Capital gains (Doh-KDVP): blocked, no XML was written (see the errors below)

Investments with a sale in 2024: 1
  XX0000000001 Synthetic Share: no problem, 1 sale, 10.0000 units

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 1
  diagnostic-0001 invalid_header, other.csv
    The file is not a Trade Republic transaction export. Required columns are missing: 15 (datetime, category, asset_class, name, symbol, price, amount, fee, tax, currency, original_amount, original_currency, fx_rate, description, transaction_id). No rows were imported.

Warnings: 0
)");
    EXPECT_EQ(folder.readOutput("summary_2024.txt"), result.mOut);
}

TEST(CliReportIntegrationTest, EmptyFileBlocksTheReport) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    // 0 bytes, not even the header.
    const auto empty = folder.writeText("empty.csv", "");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history,
                             empty});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 parse_error, empty.csv\n"
                         "    The CSV file could not be opened or did not match the expected "
                         "Trade Republic format.\n"));
}

TEST(CliReportIntegrationTest, SaleOn31DecemberIsInTheYearAndSaleOn1JanuaryIsNot) {
    // The 1 January sale is a loss (6 x 5 - 6 x 10 = -30); it would block the report if it were
    // in 2024.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-12-31T09:00:00Z,2024-12-31,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,30,120,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-01-01T09:00:00Z,2025-01-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-6,5,30,,,EUR,,,,,synthetic-sell-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Investments with a sale in 2024: 1\n"
                         "  XX0000000001 Synthetic Share: listed, 1 sale, 4.0000 units\n"));
    EXPECT_TRUE(contains(result.mOut, "Errors: 0\n"));

    const auto xml = folder.readOutput("Doh-KDVP_2024.xml");

    // Only the 4 units the 31 December sale uses are listed.
    EXPECT_TRUE(contains(xml, "<F3>4.0000</F3>"));
    EXPECT_TRUE(contains(xml, "<F6>2024-12-31</F6>"));
    EXPECT_TRUE(contains(xml, "<F7>4.0000</F7>"));
    EXPECT_FALSE(contains(xml, "2025-01-01"));
    expectValidForKdvpSchemas(xml);
}

TEST(CliReportIntegrationTest, SplitAfter31DecemberDoesNotBlockTheReport) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,30,120,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-01-15T09:00:00Z,2025-01-15,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,6,,,,,,,,,,synthetic-split-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // Only corporate actions up to 31 December of the year block.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(
        contains(result.mOut, "  XX0000000001 Synthetic Share: listed, 1 sale, 4.0000 units\n"));
    EXPECT_TRUE(contains(result.mOut, "Errors: 0\n"));
}

TEST(CliReportIntegrationTest, SaleOfAnEarlierYearUsesTheOldestPurchasesAndDoesNotBlock) {
    // 2023: 2 @ 30 and 2 @ 10 bought, 2 @ 20 sold. FIFO takes the 2 @ 30 for that sale (a loss
    // in 2023, not reported here), so the 2024 sale of 2 @ 40 uses the 2 @ 10.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2023-01-10T09:00:00Z,2023-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,30,-60,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2023-02-10T09:00:00Z,2023-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2023-06-10T09:00:00Z,2023-06-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,20,40,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,40,80,,,EUR,,,,,synthetic-sell-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(
        contains(result.mOut, "  XX0000000001 Synthetic Share: listed, 1 sale, 2.0000 units\n"));
    EXPECT_TRUE(contains(result.mOut, "Errors: 0\n"));

    const auto xml = folder.readOutput("Doh-KDVP_2024.xml");

    // One purchase row (2 @ 10 from 2023-02-10) and the sale; the 2023 sale has no row.
    EXPECT_TRUE(contains(xml,
                         "<F1>2023-02-10</F1>\n"
                         "              <F2>B</F2>\n"
                         "              <F3>2.0000</F3>\n"
                         "              <F4>10.0000</F4>\n"));
    EXPECT_FALSE(contains(xml, "2023-01-10"));
    EXPECT_FALSE(contains(xml, "2023-06-10"));
    expectValidForKdvpSchemas(xml);

    const auto fifoLists = fifoOverKdvpRows(xml);

    ASSERT_EQ(fifoLists.size(), 1U);
    // 2 x (40 - 10) = 60.
    EXPECT_EQ(fifoLists.at(0).mSaleGains, (std::vector<std::string>{"60"}));
}

TEST(CliReportIntegrationTest, ShareAndFundSalesGiveOneItemEach) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-01-11T09:00:00Z,2024-01-11,DEFAULT,TRADING,BUY,FUND,Synthetic Fund,XX0000000003,5,50,-250,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-04-10T09:00:00Z,2024-04-10,DEFAULT,TRADING,SELL,FUND,Synthetic Fund,XX0000000003,-5,60,300,,,EUR,,,,,synthetic-sell-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Investments with a sale in 2024: 2\n"
                         "  XX0000000001 Synthetic Share: listed, 1 sale, 10.0000 units\n"
                         "  XX0000000003 Synthetic Fund: listed, 1 sale, 5.0000 units\n"));

    const auto xml = folder.readOutput("Doh-KDVP_2024.xml");

    // Items in ISIN order. IsFond is false for both for now (architecture.md).
    EXPECT_TRUE(contains(xml,
                         "<ItemID>1</ItemID>\n"
                         "        <InventoryListType>PLVP</InventoryListType>\n"
                         "        <Securities>\n"
                         "          <ISIN>XX0000000001</ISIN>\n"
                         "          <Name>Synthetic Share</Name>\n"
                         "          <IsFond>false</IsFond>\n"));
    EXPECT_TRUE(contains(xml,
                         "<ItemID>2</ItemID>\n"
                         "        <InventoryListType>PLVP</InventoryListType>\n"
                         "        <Securities>\n"
                         "          <ISIN>XX0000000003</ISIN>\n"
                         "          <Name>Synthetic Fund</Name>\n"
                         "          <IsFond>false</IsFond>\n"));
    expectValidForKdvpSchemas(xml);

    const auto fifoLists = fifoOverKdvpRows(xml);

    ASSERT_EQ(fifoLists.size(), 2U);
    // Share: 10 x (30 - 10) = 200. Fund: 5 x (60 - 50) = 50.
    EXPECT_EQ(fifoLists.at(0).mSaleGains, (std::vector<std::string>{"200"}));
    EXPECT_EQ(fifoLists.at(1).mSaleGains, (std::vector<std::string>{"50"}));
}

TEST(CliReportIntegrationTest, BlockedInvestmentListsEachOfItsSales) {
    // Lots in FIFO order: 2 @ 10, 3 @ 40, 5 @ 10. The sale of 4 @ 20 uses 2 @ 10 (+20) and 2 @ 40
    // (-40); the sale of 6 @ 25 uses 1 @ 40 (-15) and 5 @ 10 (+75). No purchase lies within 30
    // days of a sale, so the loss units are assumed blocked (no answer) and the profitable ones
    // are not: both sales need split rows (#267). One warning covers both sales of the ISIN.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-01-11T09:00:00Z,2024-01-11,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,40,-120,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-01-12T09:00:00Z,2024-01-12,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,5,10,-50,,,EUR,,,,,synthetic-buy-3,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,20,80,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-05-10T09:00:00Z,2024-05-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-6,25,150,,,EUR,,,,,synthetic-sell-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // The code is listed once; each sale gets its own line and its own error.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2024 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): blocked, no XML was written (see the errors below)

Investments with a sale in 2024: 1
  XX0000000001 Synthetic Share: blocked (unsupported_tax_treatment)
    sale 2024-03-10, history.csv row 5
    sale 2024-05-10, history.csv row 6

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 2
  diagnostic-0001 unsupported_tax_treatment, XX0000000001 Synthetic Share, 2024-03-10, history.csv row 5
    The 30-day loss rule blocks only part of the loss of this sale. Rows for such a sale are not supported yet, so the sale cannot be reported. No Doh-KDVP file was written.
  diagnostic-0003 unsupported_tax_treatment, XX0000000001 Synthetic Share, 2024-05-10, history.csv row 6
    The 30-day loss rule blocks only part of the loss of this sale. Rows for such a sale are not supported yet, so the sale cannot be reported. No Doh-KDVP file was written.

Warnings: 1
  diagnostic-0002 loss_eligibility_unresolved, XX0000000001 Synthetic Share, 2024-03-10, history.csv row 5 (+1 more)
    A sale of this investment was made at a loss, and your files alone cannot decide whether the 30-day rule blocks the loss. This command cannot ask you yet, so the whole open loss was treated as not deductible (F10 false). If, within 30 days before or after the sale, you did not buy or sell this investment anywhere that is not in your files, correct it yourself in eDavki.
)");
}

TEST(CliReportIntegrationTest, HelpExplainsTheCommand) {
    const auto general = run({"--help"});

    EXPECT_EQ(general.mExitCode, kExitSuccess);
    EXPECT_TRUE(general.mErr.empty());
    EXPECT_TRUE(contains(general.mOut,
                         "  taxbroker report --year YYYY --taxpayer FILE [--self-report] --out DIR "
                         "CSV [CSV ...]\n"));

    const auto report = run({"report", "--help"});

    EXPECT_EQ(report.mExitCode, kExitSuccess);
    EXPECT_TRUE(report.mErr.empty());
    EXPECT_TRUE(contains(report.mOut, "  --self-report      Write a self-report (samoprijava)"));
    EXPECT_TRUE(contains(report.mOut, "    \"taxNumber\": \"12345678\",\n"));
    EXPECT_TRUE(contains(report.mOut,
                         "  - Private-market events (Trade Republic types "
                         "PRIVATE_MARKET_BUY, PRIVATE_MARKET_SELL\n"));
    EXPECT_TRUE(contains(report.mOut,
                         "It is also blocked when a file cannot be read, when a trade or "
                         "corporate-action row of\nany file is rejected"));
    EXPECT_TRUE(contains(report.mOut,
                         "A private-market row that cannot be read is listed with\n"
                         "    its error, but it does not block the report.\n"));
    EXPECT_TRUE(contains(report.mOut,
                         "  0  The XML was written, or no sale in the year needs a Doh-KDVP "
                         "form.\n"
                         "  1  The report is blocked: the errors are in the summary. No XML was "
                         "written.\n"));
}

TEST(CliReportIntegrationTest, NoArgumentsPrintTheHelpAsAnError) {
    const auto result = run({});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_TRUE(contains(result.mErr, "Run 'taxbroker report --help' for the details.\n"));
}

TEST(CliReportIntegrationTest, ShortHelpOptionPrintsTheGeneralHelp) {
    const auto result = run({"-h"});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_TRUE(contains(result.mOut, "Run 'taxbroker report --help' for the details.\n"));
}

TEST(CliReportIntegrationTest, HelpCommandPrintsTheGeneralHelp) {
    const auto result = run({"help"});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_TRUE(contains(result.mOut, "Run 'taxbroker report --help' for the details.\n"));
}

TEST(CliReportIntegrationTest, ShortHelpOptionOfReportPrintsTheReportHelp) {
    const auto result = run({"report", "-h"});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_TRUE(contains(result.mOut, "  -h, --help         Show this help.\n"));
}

TEST(CliReportIntegrationTest, HelpAfterOtherOptionsPrintsTheReportHelpAndRunsNothing) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history,
                             "--help"});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_TRUE(contains(result.mOut, "  -h, --help         Show this help.\n"));
    EXPECT_FALSE(std::filesystem::exists(folder.outputDirectory()));
}

TEST(CliReportIntegrationTest, UnknownCommandFails) {
    const auto result = run({"reprot"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: unknown command reprot.\nRun 'taxbroker --help' for the commands.\n");
}

TEST(CliReportIntegrationTest, CsvPathAsTheCommandPrintsOnlyItsFileName) {
    // `report` was forgotten: the folders of the path are never printed.
    const auto result = run({"/home/synthetic/exports/export.csv"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: unknown command export.csv.\nRun 'taxbroker --help' for the commands.\n");
}

TEST(CliReportIntegrationTest, MissingYearFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result =
        run({"report", "--taxpayer", taxpayer, "--out", folder.outputDirectory(), history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr,
              "taxbroker: --year is missing.\nRun 'taxbroker report --help' for the usage.\n");
    EXPECT_FALSE(std::filesystem::exists(folder.outputDirectory()));
}

TEST(CliReportIntegrationTest, YearBefore2000Fails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "1999",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n"
              "Run 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, YearGivenTwiceFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year is given twice.\nRun 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, OptionWithoutValueFails) {
    const auto result = run({"report", "--year"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year needs a value.\nRun 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, UnknownOptionFails) {
    const auto result = run({"report", "--yaer", "2024"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: Unknown option --yaer.\nRun 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, OptionWithAnEqualsSignPrintsNoValue) {
    // Values follow their option after a space; the folder after "=" is never printed.
    const auto result = run({"report", "--out=/home/synthetic/reports"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(
        result.mErr,
        "taxbroker: Unknown option --out=...\nRun 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, MissingTaxpayerOptionFails) {
    const TemporaryCliFolder folder;
    const auto history = folder.writeCsv("history.csv", {});

    const auto result =
        run({"report", "--year", "2024", "--out", folder.outputDirectory(), history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --taxpayer is missing.\nRun 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, MissingOutputOptionFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report", "--year", "2024", "--taxpayer", taxpayer, history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --out is missing.\nRun 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, NoCsvFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);

    const auto result = run(
        {"report", "--year", "2024", "--taxpayer", taxpayer, "--out", folder.outputDirectory()});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: Give at least one Trade Republic CSV file.\n"
              "Run 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, CsvThatDoesNotExistFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             folder.outputDirectory() + "/../missing.csv"});

    // Only the file name is printed, never the folder.
    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file missing.csv does not exist or is not a file.\n"
              "Run 'taxbroker report --help' for the usage.\n");
    EXPECT_FALSE(std::filesystem::exists(folder.outputDirectory()));
}

TEST(CliReportIntegrationTest, TaxpayerFileThatDoesNotExistFails) {
    const TemporaryCliFolder folder;
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             folder.outputDirectory() + "/../taxpayer.json",
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The taxpayer file taxpayer.json does not exist or is not a file.\n"
              "Run 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, InvalidTaxpayerFileFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer =
        folder.writeText("taxpayer.json",
                         R"({"taxNumber": "12345678", "isResident": true, "filingType": "S"})");
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: the taxpayer file taxpayer.json is not valid: \"filingType\" is not "
              "read from the taxpayer file: the report is an original return, or a self-report "
              "with --self-report.\n"
              "Run 'taxbroker report --help' for the usage.\n");
    EXPECT_FALSE(std::filesystem::exists(folder.outputDirectory()));
}

TEST(CliReportIntegrationTest, OutputPathThatIsAFileFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result =
        run({"report", "--year", "2024", "--taxpayer", taxpayer, "--out", history, history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --out must be a folder, not a file.\n"
              "Run 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, YearWithAPlusSignFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "+2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n"
              "Run 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, YearWithLettersAfterTheDigitsFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024x",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n"
              "Run 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, YearAfter9999Fails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "10000",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --year must be a year from 2000 to 9999.\n"
              "Run 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, Year2000IsAcceptedAndAFileWithOnlyTheHeaderGivesNoData) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2000",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2000.xml"));
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2000 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): no data, no sale in 2000 needs a Doh-KDVP form

Investments with a sale in 2000: 0

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 0

Warnings: 0
)");
}

TEST(CliReportIntegrationTest, Year9999IsAccepted) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "9999",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): no data, no sale in 9999 needs a Doh-KDVP "
                         "form\n"));
    EXPECT_TRUE(folder.hasOutput("summary_9999.txt"));
}

TEST(CliReportIntegrationTest, TaxpayerGivenTwiceFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(
        result.mErr,
        "taxbroker: --taxpayer is given twice.\nRun 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, OutputGivenTwiceFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --out is given twice.\nRun 'taxbroker report --help' for the usage.\n");
    EXPECT_FALSE(std::filesystem::exists(folder.outputDirectory()));
}

TEST(CliReportIntegrationTest, CsvPathThatIsAFolderFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    std::filesystem::create_directory(folder.outputDirectory());

    // The output folder is given as the CSV.
    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             folder.outputDirectory()});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file out does not exist or is not a file.\n"
              "Run 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, TaxpayerPathThatIsAFolderFails) {
    const TemporaryCliFolder folder;
    const auto history = folder.writeCsv("history.csv", {});
    std::filesystem::create_directory(folder.outputDirectory());

    // The output folder is given as the taxpayer file.
    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             folder.outputDirectory(),
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The taxpayer file out does not exist or is not a file.\n"
              "Run 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, OptionsMayFollowTheCsvPath) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             history,
                             "--out",
                             folder.outputDirectory(),
                             "--self-report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_TRUE(contains(result.mOut,
                         "TaxBrokerReport: Doh-KDVP 2024 (self-report)\n"
                         "Input files: 1\n"
                         "  source-0001 history.csv\n"));
    EXPECT_TRUE(folder.hasOutput("Doh-KDVP_2024.xml"));
}

TEST(CliReportIntegrationTest, MissingOutputFolderIsCreatedWithItsParentFolders) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    // Neither out nor out/2024 nor out/2024/report exists yet.
    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory() + "/2024/report",
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(folder.hasOutput("2024/report/Doh-KDVP_2024.xml"));
    EXPECT_TRUE(folder.hasOutput("2024/report/summary_2024.txt"));
}

TEST(CliReportIntegrationTest, SaleOfOnlyLongHeldUnitsGivesNoDataAndListsThem) {
    // Sale in 2025, so 15 years: both units from 2005 are past the holding period and tax free.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2005-04-01T09:00:00Z,2005-04-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-06-02T09:00:00Z,2025-06-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,30,60,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2025 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): no data, no sale in 2025 needs a Doh-KDVP form

Investments with a sale in 2025: 1
  XX0000000001 Synthetic Share: only units past the holding period, not in the XML

Units past the holding period (tax free, not in the XML): 1
  XX0000000001 Synthetic Share: sale 2025-06-02, 2.0000 units

Private-market events skipped (only a cash amount, no units): 0

Errors: 0

Warnings: 0
)");
}

TEST(CliReportIntegrationTest, DividendErrorIsListedApartAndDoesNotBlock) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
            // amount is "ten", not a number.
            R"(2024-04-01T09:00:00Z,2024-04-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,ten,,,EUR,,,,,synthetic-dividend-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // The error affects Doh-Div only, so Doh-KDVP is still generated.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 0\n\nWarnings: 0\n\n"
                         "Not about Doh-KDVP (dividends, interest and skipped private-market "
                         "rows): 1\n"
                         "  diagnostic-0001 error invalid_value, history.csv row 4\n"));
}

TEST(CliReportIntegrationTest, DividendWarningIsListedApartWithItsSeverity) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
            // name is blank.
            R"(2024-04-01T09:00:00Z,2024-04-01,DEFAULT,CASH,DIVIDEND,STOCK,,XX0000000001,,,10,,,EUR,,,,,synthetic-dividend-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // The warning affects Doh-Div only; the mixed list names its severity.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 0\n\nWarnings: 0\n\n"
                         "Not about Doh-KDVP (dividends, interest and skipped private-market "
                         "rows): 1\n"
                         "  diagnostic-0001 warning missing_field, history.csv row 4\n"));
}

TEST(CliReportIntegrationTest, DoubleDashEndsTheOptions) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             "--",
                             history,
                             "--self-report"});

    // After "--", "--self-report" is a CSV path, not the option.
    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: The CSV file --self-report does not exist or is not a file.\n"
              "Run 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, SelfReportGivenTwiceFails) {
    const auto result = run({"report", "--self-report", "--self-report"});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_EQ(result.mErr,
              "taxbroker: --self-report is given twice.\n"
              "Run 'taxbroker report --help' for the usage.\n");
}

TEST(CliReportIntegrationTest, OutputFolderThatCannotBeCreatedFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv("history.csv", {});

    // The output folder would be inside a file.
    const auto result = run(
        {"report", "--year", "2024", "--taxpayer", taxpayer, "--out", taxpayer + "/out", history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: the output folder cannot be created.\n");
}

TEST(CliReportIntegrationTest, XmlThatCannotBeWrittenFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    // A folder where the XML file should go.
    std::filesystem::create_directories(folder.outputDirectory() + "/Doh-KDVP_2024.xml");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: Doh-KDVP_2024.xml cannot be written.\n");
    EXPECT_FALSE(folder.hasOutput("summary_2024.txt"));
}

TEST(CliReportIntegrationTest, OldXmlThatCannotBeDeletedFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
        });
    // A folder that is not empty where the old XML file would be, so it cannot be deleted.
    std::filesystem::create_directories(folder.outputDirectory() + "/Doh-KDVP_2025.xml");
    (void)folder.writeText("out/Doh-KDVP_2025.xml/keep.txt", "keep");

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: the old Doh-KDVP_2025.xml cannot be deleted.\n");
}

TEST(CliReportIntegrationTest, SummaryThatCannotBeWrittenFails) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
        });
    // A folder where the summary file should go.
    std::filesystem::create_directories(folder.outputDirectory() + "/summary_2025.txt");

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: summary_2025.txt cannot be written.\n");
}

TEST(CliReportIntegrationTest, SummaryThatCannotBeWrittenInAGeneratedYearWritesNoXml) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    // A folder where the summary file should go.
    std::filesystem::create_directories(folder.outputDirectory() + "/summary_2024.txt");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // The XML could be written, but exit 2 means nothing was written.
    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: summary_2024.txt cannot be written.\n");
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml.tmp"));
}

TEST(CliReportIntegrationTest, SummaryThatCannotBeWrittenKeepsTheOldXmlOfABlockedYear) {
    // Buy 10 @ 30, sell 10 @ 20: a loss, so the year is blocked.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    std::filesystem::create_directory(folder.outputDirectory());
    (void)folder.writeText("out/Doh-KDVP_2025.xml", "an old file of an earlier run");
    // A folder where the summary file should go.
    std::filesystem::create_directory(folder.outputDirectory() + "/summary_2025.txt");

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    // A blocked run deletes the old XML, but not when it fails with exit 2.
    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: summary_2025.txt cannot be written.\n");
    EXPECT_EQ(folder.readOutput("Doh-KDVP_2025.xml"), "an old file of an earlier run");
}

TEST(CliReportIntegrationTest, TemporarySummaryThatCannotBeWrittenChangesNothing) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    std::filesystem::create_directory(folder.outputDirectory());
    (void)folder.writeText("out/Doh-KDVP_2024.xml", "an old file of an earlier run");
    (void)folder.writeText("out/summary_2024.txt", "an old summary of an earlier run");
    // A folder where the summary is written before it replaces the old one.
    std::filesystem::create_directory(folder.outputDirectory() + "/summary_2024.txt.tmp");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: summary_2024.txt cannot be written.\n");

    // The files of the earlier run are unchanged, and the new XML's temporary file is deleted.
    EXPECT_EQ(folder.readOutput("Doh-KDVP_2024.xml"), "an old file of an earlier run");
    EXPECT_EQ(folder.readOutput("summary_2024.txt"), "an old summary of an earlier run");
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml.tmp"));
    // The folder in the way was not made by the run, so it stays.
    EXPECT_TRUE(folder.hasOutput("summary_2024.txt.tmp"));
}

TEST(CliReportIntegrationTest, TemporaryXmlThatCannotBeWrittenChangesNothing) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    // A folder where the XML is written before it replaces an old one.
    std::filesystem::create_directories(folder.outputDirectory() + "/Doh-KDVP_2024.xml.tmp");

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitFailure);
    EXPECT_TRUE(result.mOut.empty());
    EXPECT_EQ(result.mErr, "taxbroker: Doh-KDVP_2024.xml cannot be written.\n");
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_FALSE(folder.hasOutput("summary_2024.txt"));
    EXPECT_FALSE(folder.hasOutput("summary_2024.txt.tmp"));
}

} // namespace
