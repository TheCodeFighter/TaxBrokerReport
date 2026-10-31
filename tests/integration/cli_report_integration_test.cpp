#include "../support/kdvp_row_fifo.hpp"
#include "../support/xsd_validation.hpp"
#include "cli/report_command.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

    /// The names in a subfolder ("" for the folder itself), sorted.
    [[nodiscard]] std::vector<std::string> fileNames(std::string_view aSubfolder) const {
        std::vector<std::string> names;

        for (const auto& entry : std::filesystem::directory_iterator{mDirectory / aSubfolder})
        {
            names.push_back(entry.path().filename().string());
        }

        std::sort(names.begin(), names.end());

        return names;
    }

  private:
    std::filesystem::path mDirectory;
};

/// Runs the command with aInput as what the user types; without it the input ends at once.
CliRun run(const std::vector<std::string>& aArguments, std::string_view aInput = "") {
    std::istringstream in{std::string{aInput}};
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = runCli(aArguments, in, out, err);

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
    // The split needs its ratio; the input ends at once, so the question stays open.
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked (corporate_action_ambiguous)\n"
                         "    sale 2025-03-10, history.csv row 4\n"));
    // The error points at the split, not at the sale.
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 corporate_action_ambiguous, XX0000000001 Synthetic "
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

    // Without an answer the split blocks, also one after the sale: the input ends at once.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): blocked, no XML was written (see the errors "
                         "below)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked (corporate_action_ambiguous)\n"
                         "    sale 2025-03-10, history.csv row 3\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  diagnostic-0001 corporate_action_ambiguous, XX0000000001 Synthetic "
                         "Share, 2025-09-01, history.csv row 4\n"));
}

TEST(CliReportIntegrationTest, ASplitAnsweredAfterTheSaleOfTheYearGivesTheReport) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-09-01T09:00:00Z,2025-09-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,1,,,,,,,,,,synthetic-split-1,,,,)",
        });

    // The ratio 2 : 1, an empty line for the broker's date, and yes to the preview.
    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "2 : 1\n\nyes\n");

    // Up to the sale the split would come after 2 units, from the day after it after 1 unit.
    // With the row date 1 unit becomes 2. The March sale used a lot the split had not changed,
    // so its rows are written: 1 unit bought at EUR 10, 1 sold at EUR 30.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Effective date from the announcement (YYYY-MM-DD, 2025-01-11 to "
                         "2025-03-10 with 2.0000 units, or 2025-03-11 to 2025-12-31 with 1.0000 "
                         "units), or empty for 2025-09-01:\n"
                         "After it you hold 2.0000 units (2 : 1 from 2025-09-01).\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2025.xml\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 0\n\nWarnings: 1\n"
                         "  diagnostic-0001 corporate_action_applied, XX0000000001 Synthetic "
                         "Share, 2025-09-01, history.csv row 4\n"
                         "    The corporate action was applied with the ratio 2 new for 1 old "
                         "shares from 2025-09-01, as you entered it. The broker did not supply "
                         "this ratio.\n"));
    ASSERT_TRUE(folder.hasOutput("Doh-KDVP_2025.xml"));

    const auto xml = folder.readOutput("Doh-KDVP_2025.xml");

    EXPECT_TRUE(contains(xml,
                         "<Purchase>\n"
                         "              <F1>2025-01-10</F1>\n"
                         "              <F2>B</F2>\n"
                         "              <F3>1.0000</F3>\n"
                         "              <F4>10.0000</F4>\n"));
    EXPECT_TRUE(contains(xml,
                         "<Sale>\n"
                         "              <F6>2025-03-10</F6>\n"
                         "              <F7>1.0000</F7>\n"
                         "              <F9>30.0000</F9>\n"));
}

TEST(CliReportIntegrationTest, ADateWithoutUnitsIsRejectedAndAskedAgain) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,12,12,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-05-01T09:00:00Z,2024-05-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,1,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,30,60,,,EUR,,,,,synthetic-sell-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "2 : 1\n2024-04-01\ncancel\n");

    // From 2024-03-02 to 2024-05-01 no unit is held (sold in March, bought again in May). Such a
    // date has no preview; the session rejects the date, and the question is asked again.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to "
                         "2024-03-01 with 1.0000 units, or 2024-05-02 to 2025-03-10 with 1.0000 "
                         "units), or empty for 2024-06-01:\n"
                         "On the effective date in the answer to request-0001, no units are held "
                         "just before the action. Check the announcement, or close the "
                         "question.\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked (corporate_action_ambiguous)\n"));
}

TEST(CliReportIntegrationTest, ARatioThatLeavesTooFewUnitsForALaterSaleIsAskedAgain) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-05-01T09:00:00Z,2024-05-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-5,12,60,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,1,,,,,,,,,,synthetic-split-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "1 : 10\n2024-03-01\nyes\ncancel\n");

    // On 2024-03-01 the action comes before the sale of 5 units on 2024-05-01: 10 x 1 / 10 = 1
    // unit is too few, so the answer is rejected and the question is asked again.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "After it you hold 1.0000 units (1 : 10 from 2024-03-01).\n"
                         "Is this right? (yes or no):\n"
                         "With the ratio and the effective date in the answer to request-0001, a "
                         "later sale needs more units than are held after the action. Check the "
                         "ratio and the date in the announcement.\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked (corporate_action_ambiguous)\n"));
}

TEST(CliReportIntegrationTest, ASaleWithTooFewUnitsBeforeASplitRowAsksForTheSplitDate) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-20,6,120,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-06-03T09:00:00Z,2024-06-03,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,10,,,,,,,,,,synthetic-split-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "2 : 1\n2024-05-30\nyes\n");

    // The sale of 20 units needs more than the 10 bought, and the split row follows 2 days
    // later: the split may have taken effect before the sale. 2 : 1 from 2024-05-30 makes the
    // 10 units 20, so the sale fits and the report is written: the purchase row lists the 20
    // units at EUR 10 / 2 = EUR 5.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "  The sale on 2024-06-01 needs more units than you held. If the action "
                         "took effect on\n"
                         "  or before that sale, enter it; if not, cancel: earlier purchases are "
                         "then missing.\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to "
                         "2024-06-01):\n"
                         "After it you hold 20.0000 units (2 : 1 from 2024-05-30).\n"));
    EXPECT_TRUE(
        contains(result.mOut, "  XX0000000001 Synthetic Share: listed, 1 sale, 20.0000 units\n"));
    EXPECT_FALSE(contains(result.mOut, "incomplete_history"));
}

TEST(CliReportIntegrationTest, ASaleWithTooFewUnitsBeforeASplitRowBlocksWithoutInput) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-20,6,120,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-06-03T09:00:00Z,2024-06-03,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,10,,,,,,,,,,synthetic-split-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--no-input",
                             "--out",
                             folder.outputDirectory(),
                             history});

    // The question stays open; its error names the sale and the 10 days.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked (corporate_action_ambiguous)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "    A sale of this investment on 2024-06-01 needs more units than the "
                         "earlier purchases left, and a corporate action, such as a split, follows "
                         "it within 10 days."));
    EXPECT_FALSE(contains(result.mOut, "incomplete_history"));
}

TEST(CliReportIntegrationTest, ACancelledQuestionBeforeASaleWithTooFewUnitsGivesIncompleteHistory) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-20,6,120,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-06-03T09:00:00Z,2024-06-03,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,10,,,,,,,,,,synthetic-split-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "cancel\n");

    // The sale of 20 units needs more than the 10 bought, and the split row follows 2 days later.
    // The question says that a cancel means missing purchases, so the sale gets the error it would
    // have without the split row.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "  The sale on 2024-06-01 needs more units than you held. If the action "
                         "took effect on\n"
                         "  or before that sale, enter it; if not, cancel: earlier purchases are "
                         "then missing.\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked (incomplete_history)\n"
                         "    sale 2024-06-01, history.csv row 3\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 incomplete_history, XX0000000001 Synthetic Share, "
                         "2024-06-01, history.csv row 3\n"));
    EXPECT_FALSE(contains(result.mOut, "corporate_action_ambiguous"));
    EXPECT_TRUE(contains(result.mOut,
                         "Answered in this run: 1\n"
                         "  XX0000000001 Synthetic Share, corporate action on 2024-06-03: "
                         "cancelled\n"));
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

// Questions in the terminal

TEST(CliReportIntegrationTest, ALossQuestionAnsweredNoWritesTheLossAsDeductible) {
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
                             history},
                            "no\n");

    // 10 x 20 - 10 x 30 = -100: a loss. The window is 30 days before and after the sale
    // (2025-02-08 to 2025-04-09); the purchase of 2025-01-10 is outside it, so the files cannot
    // decide and the command asks. The answer no keeps the loss of all 10 units (F10 true).
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_EQ(result.mOut,
              R"(Question request-0001: the 30-day loss rule for XX0000000001 Synthetic Share
  Your files cannot decide the loss of 1 sale:
    sale 2025-03-10: 10.0000 units, 10.0000 at a loss, 10.0000 open; window 2025-02-08 to 2025-04-09, no purchase in your files counts against it
  Within 30 days before or after the sale, did you also buy or sell this investment
  anywhere that is not in your files (another broker, another account, or after the
  export)? yes: the open loss is not deductible (F10 false). no: it is deductible.
  cancel: no answer, and the sales cannot be reported.
Your answer (yes, no or cancel):

TaxBrokerReport: Doh-KDVP 2025 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2025.xml

Investments with a sale in 2025: 1
  XX0000000001 Synthetic Share: listed, 1 sale, 10.0000 units

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 0

Warnings: 0

Answered in this run: 1
  XX0000000001 Synthetic Share, 30-day loss rule: no, not bought or sold anywhere else (the open loss is deductible)
)");
    EXPECT_TRUE(result.mOut.ends_with(folder.readOutput("summary_2025.txt")));
    EXPECT_EQ(folder.readOutput("summary_2025.txt").rfind("TaxBrokerReport: Doh-KDVP 2025", 0), 0U);

    const auto xml = folder.readOutput("Doh-KDVP_2025.xml");

    EXPECT_TRUE(contains(xml, "<F10>true</F10>"));
    EXPECT_FALSE(contains(xml, "<F10>false</F10>"));
    expectValidForKdvpSchemas(xml);
}

TEST(CliReportIntegrationTest, ALossQuestionAnsweredYesWritesTheLossAsBlocked) {
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
                             history},
                            "YES\n");

    // The answer yes (in any case) blocks the loss of all 10 open units (F10 false).
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: listed, 1 sale, 10.0000 units, 10.0000 "
                         "units with a loss blocked by the 30-day rule (F10 false)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 0\n"
                         "\n"
                         "Warnings: 0\n"
                         "\n"
                         "Answered in this run: 1\n"
                         "  XX0000000001 Synthetic Share, 30-day loss rule: yes, bought or sold "
                         "elsewhere (the open loss is not deductible, F10 false)\n"));

    const auto xml = folder.readOutput("Doh-KDVP_2025.xml");

    EXPECT_TRUE(contains(xml, "<F10>false</F10>"));
    expectValidForKdvpSchemas(xml);
}

TEST(CliReportIntegrationTest, AJanuarySplitInADecemberLossWindowIsAskedThenTheLoss) {
    // 10 bought, 4 sold at a loss on 2024-12-10, a split row on 2025-01-05 and 4 bought on
    // 2025-01-08. The split ratio is asked first: 2 : 1 from the row date (6 units, 12 after).
    // The 4 new units are 2 units of the sale, so 2 loss units are open; yes blocks them too.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-12-10T09:00:00Z,2024-12-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,8,32,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-01-05T09:00:00Z,2025-01-05,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,6,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2025-01-08T09:00:00Z,2025-01-08,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,4,4,-16,,,EUR,,,,,synthetic-buy-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "2 : 1\n\nyes\nyes\n");

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Effective date from the announcement (YYYY-MM-DD, 2024-03-02 to "
                         "2024-12-10 with 10.0000 units, or 2024-12-11 to 2025-01-08 with 6.0000 "
                         "units, or 2025-01-09 to 2025-01-30 with 10.0000 units), or empty for "
                         "2025-01-05:"));
    // 4 units bought after 2 : 1 are 2 units of the sale.
    EXPECT_TRUE(contains(result.mOut, "in the units of this sale: 2025-01-08 2.0000 units\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: listed, 1 sale, 4.0000 units, 4.0000 "
                         "units with a loss blocked by the 30-day rule (F10 false)\n"));

    const auto xml = folder.readOutput("Doh-KDVP_2024.xml");

    EXPECT_TRUE(contains(xml, "<F10>false</F10>"));
    expectValidForKdvpSchemas(xml);
}

TEST(CliReportIntegrationTest, AJanuarySaleWithTooFewUnitsBeforeTheJanuarySplitIsAsked) {
    // 10 bought, 4 sold at a loss on 2024-12-10, 12 sold on 2025-01-03, a split row on
    // 2025-01-05: only 6 units before the January sale, so the split may have taken effect
    // before it. The question ends at that sale and stays open.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-12-10T09:00:00Z,2024-12-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,8,32,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-01-03T09:00:00Z,2025-01-03,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-12,4,48,,,EUR,,,,,synthetic-sell-2,,,,)",
            R"(2025-01-05T09:00:00Z,2025-01-05,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,12,,,,,,,,,,synthetic-split-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--no-input",
                             "--out",
                             folder.outputDirectory(),
                             history});

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(result.mErr.empty());
    EXPECT_TRUE(contains(result.mOut,
                         "    A sale of this investment on 2025-01-03 needs more units than the "
                         "earlier purchases left, and a corporate action, such as a split, follows "
                         "it within 10 days."));
}

TEST(CliReportIntegrationTest, AJanuarySaleWithTooFewUnitsLongBeforeTheJanuarySplitIsNotAsked) {
    // 10 bought, 4 sold at a loss on 2024-12-30, 12 sold on 2025-01-02 with only 6 held, a split
    // row on 2025-01-20, inside the loss window (to 2025-01-29). The row is 18 days after the
    // January sale, more than the 10 days a broker may book late, so the sale points to missing
    // purchases: no units are known just before the split, and no question is asked.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-12-30T09:00:00Z,2024-12-30,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,8,32,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-01-02T09:00:00Z,2025-01-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-12,4,48,,,EUR,,,,,synthetic-sell-2,,,,)",
            R"(2025-01-20T09:00:00Z,2025-01-20,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,12,,,,,,,,,,synthetic-split-1,,,,)",
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
    EXPECT_FALSE(contains(result.mOut, "Question request-0001"));
    EXPECT_TRUE(
        contains(result.mOut,
                 "  XX0000000001 Synthetic Share: blocked (corporate_action_missing_position)\n"
                 "    sale 2024-12-30, history.csv row 3\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 corporate_action_missing_position, XX0000000001 "
                         "Synthetic Share, 2025-01-20, history.csv row 5\n"));
}

TEST(CliReportIntegrationTest, TwoJanuarySplitsInADecemberLossWindowAreAskedInDateOrder) {
    // 10 bought, 4 sold at a loss on 2024-12-10 (window to 2025-01-09), a split row on 2025-01-03,
    // 2 bought on 2025-01-05 and a second split row on 2025-01-07. The first split is asked
    // first; the second is asked after it, with the first applied to the units before it.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-12-10T09:00:00Z,2024-12-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,8,32,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-01-03T09:00:00Z,2025-01-03,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,6,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2025-01-05T09:00:00Z,2025-01-05,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,4,-8,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2025-01-07T09:00:00Z,2025-01-07,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,14,,,,,,,,,,synthetic-split-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "2 : 1\n\nyes\n2 : 1\n\nyes\nyes\n");

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    // First split: 10 - 4 = 6 units at its row; 6 + 2 = 8 after the January purchase, up to the
    // day before the second row.
    EXPECT_TRUE(contains(result.mOut,
                         "Question request-0001: a corporate action of XX0000000001 Synthetic "
                         "Share on 2025-01-03\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Effective date from the announcement (YYYY-MM-DD, 2024-03-02 to "
                         "2024-12-10 with 10.0000 units, or 2024-12-11 to 2025-01-05 with 6.0000 "
                         "units, or 2025-01-06 to 2025-01-06 with 8.0000 units), or empty for "
                         "2025-01-03:\n"
                         "After it you hold 12.0000 units (2 : 1 from 2025-01-03).\n"));
    // Second split: 6 units are 12 after 2 : 1 from 2025-01-03, plus the 2 bought on 2025-01-05
    // are 14 at its row.
    EXPECT_TRUE(contains(result.mOut,
                         "Question request-0001: a corporate action of XX0000000001 Synthetic "
                         "Share on 2025-01-07\n"));
    EXPECT_TRUE(contains(result.mOut, "  You held 14.0000 units just before it.\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Effective date from the announcement (YYYY-MM-DD, 2025-01-04 to "
                         "2025-01-05 with 12.0000 units, or 2025-01-06 to 2025-01-30 with 14.0000 "
                         "units), or empty for 2025-01-07:\n"
                         "After it you hold 28.0000 units (2 : 1 from 2025-01-07).\n"));
    // The 2 units bought after the first 2 : 1 and before the second are 1 unit of the sale, so
    // 3 of the 4 loss units are open; yes blocks them too.
    EXPECT_TRUE(contains(result.mOut,
                         "    sale 2024-12-10: 4.0000 units, 4.0000 at a loss, 3.0000 open; window "
                         "2024-11-10 to 2025-01-09, purchases in your files that count against it, "
                         "in the units of this sale: 2025-01-05 1.0000 units\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: listed, 1 sale, 4.0000 units, 4.0000 "
                         "units with a loss blocked by the 30-day rule (F10 false)\n"));

    const auto xml = folder.readOutput("Doh-KDVP_2024.xml");

    EXPECT_TRUE(contains(xml, "<F10>false</F10>"));
    expectValidForKdvpSchemas(xml);
}

TEST(CliReportIntegrationTest, AJanuarySplitWithAnOpenLossAnsweredNoGivesTwoSaleRows) {
    // 10 bought, 4 sold at a loss on 2024-12-10, split row on 2025-01-05 answered 2 : 1, 4 bought
    // on 2025-01-08. No keeps the 2 open units deductible, but the other 2 are blocked by the
    // January purchase (4 new units are 2 units of the sale): a partly blocked sale, written as
    // two sale rows of 2 units, the blocked units first.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-12-10T09:00:00Z,2024-12-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,8,32,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-01-05T09:00:00Z,2025-01-05,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,6,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2025-01-08T09:00:00Z,2025-01-08,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,4,4,-16,,,EUR,,,,,synthetic-buy-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "2 : 1\n\nyes\nno\n");

    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: listed, 1 sale, 4.0000 units, 2.0000 "
                         "units with a loss blocked by the 30-day rule (F10 false)\n"));

    const auto xml = folder.readOutput("Doh-KDVP_2024.xml");

    EXPECT_TRUE(contains(xml, R"xml(            <Sale>
              <F6>2024-12-10</F6>
              <F7>2.0000</F7>
              <F9>8.0000</F9>
              <F10>false</F10>
            </Sale>
            <F8>2.0000</F8>
)xml"));
    EXPECT_TRUE(contains(xml, R"xml(            <Sale>
              <F6>2024-12-10</F6>
              <F7>2.0000</F7>
              <F9>8.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
)xml"));
    expectValidForKdvpSchemas(xml);

    const auto fifoLists = fifoOverKdvpRows(xml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: the 4 units the sale uses, 4 - 2 = 2, 2 - 2 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"4", "2", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"4", "2", "0"}));
    // 2 x (8 - 10) = -4 for each of the two sale rows.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"-4", "-4"}));
}

TEST(CliReportIntegrationTest, AnInvalidLossAnswerIsAskedAgain) {
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
                             history},
                            "maybe\n\ny\n  no  \n");

    // "maybe", an empty line and "y" are not answers; " no " with spaces around it is.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Your answer (yes, no or cancel):\n"
                         "Answer yes, no or cancel.\n"
                         "Your answer (yes, no or cancel):\n"
                         "Answer yes, no or cancel.\n"
                         "Your answer (yes, no or cancel):\n"
                         "Answer yes, no or cancel.\n"
                         "Your answer (yes, no or cancel):\n"
                         "\n"
                         "TaxBrokerReport: Doh-KDVP 2025 (original return)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share, 30-day loss rule: no, not bought or "
                         "sold anywhere else (the open loss is deductible)\n"));
}

TEST(CliReportIntegrationTest, ALossQuestionWithNoInputBlocksAndListsTheRequest) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    // The input has an answer, but --no-input never reads it.
    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--no-input",
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "no\n");

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_EQ(result.mOut, R"(TaxBrokerReport: Doh-KDVP 2025 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): blocked, no XML was written (see the errors below)

Investments with a sale in 2025: 1
  XX0000000001 Synthetic Share: blocked (loss_eligibility_unresolved)
    sale 2025-03-10, history.csv row 3

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 1
  diagnostic-0001 loss_eligibility_unresolved, XX0000000001 Synthetic Share, 2025-03-10, history.csv row 3
    Part of this sale was sold at a loss. Answer whether, within 30 days before or after the sale, you also bought or sold this investment anywhere that is not in your files: at another broker, in another account, or after the last day of your export. If you did, the whole open loss is treated as not deductible, and you correct it yourself in eDavki. Until you answer, no Doh-KDVP file is written.

Warnings: 0

Open questions: 1
  request-0001 loss_eligibility_confirmation_required, XX0000000001 Synthetic Share
  Run the command again in a terminal (without --no-input) to answer them.
)");
    EXPECT_EQ(folder.readOutput("summary_2025.txt"), result.mOut);
}

TEST(CliReportIntegrationTest, ALossQuestionAtTheEndOfTheInputStaysOpen) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    // The input ends before an answer (Ctrl-D in a terminal).
    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "");

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Your answer (yes, no or cancel):\n"
                         "Input ended: the open questions are listed in the summary.\n"
                         "\n"
                         "TaxBrokerReport: Doh-KDVP 2025 (original return)\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Open questions: 1\n"
                         "  request-0001 loss_eligibility_confirmation_required, XX0000000001 "
                         "Synthetic Share\n"));
    EXPECT_FALSE(contains(result.mOut, "Answered in this run"));
}

TEST(CliReportIntegrationTest, ACancelledLossQuestionBlocksTheReport) {
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
                             history},
                            "cancel\n");

    // A cancel closes the question: the sale gets an error without a request.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 loss_eligibility_unresolved, XX0000000001 Synthetic "
                         "Share, 2025-03-10, history.csv row 3\n"
                         "    The question about the loss of this sale was closed without an "
                         "answer, so the sale cannot be reported. No Doh-KDVP file was "
                         "written.\n"));
    EXPECT_FALSE(contains(result.mOut, "Open questions"));
    EXPECT_TRUE(contains(result.mOut,
                         "Answered in this run: 1\n"
                         "  XX0000000001 Synthetic Share, 30-day loss rule: cancelled\n"));
}

TEST(CliReportIntegrationTest, ALossQuestionWithAKnownPurchaseListsIt) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-03-20T09:00:00Z,2025-03-20,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,21,-63,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2025-03-25T09:00:00Z,2025-03-25,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,21,-21,,,EUR,,,,,synthetic-buy-3,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "yes\n");

    // The purchases of 3 units on 2025-03-20 and 1 unit on 2025-03-25 lie in the window, so they
    // block the loss of 3 + 1 = 4 of the 10 units; 10 - 4 = 6 stay open. The answer yes blocks
    // those 6 too, so all 10 are blocked (F10 false) and the row is written whole.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "    sale 2025-03-10: 10.0000 units, 10.0000 at a loss, 6.0000 open; "
                         "window 2025-02-08 to 2025-04-09, purchases in your files that count "
                         "against it, in the units of this sale: 2025-03-20 3.0000 units, "
                         "2025-03-25 1.0000 units\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: listed, 1 sale, 10.0000 units, 10.0000 "
                         "units with a loss blocked by the 30-day rule (F10 false)\n"));
}

TEST(CliReportIntegrationTest, ALossQuestionOfTwoSalesListsBothAndOneAnswerCoversThem) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,20,80,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-06-10T09:00:00Z,2025-06-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-6,25,150,,,EUR,,,,,synthetic-sell-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "no\n");

    // Both sales lose (4 x 20 - 4 x 30 = -40, 6 x 25 - 6 x 30 = -30) and no purchase lies in a
    // window: 2025-06-10 is 92 days after 2025-03-10. One question, one answer.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "  Your files cannot decide the loss of 2 sales:\n"
                         "    sale 2025-03-10: 4.0000 units, 4.0000 at a loss, 4.0000 open; "
                         "window 2025-02-08 to 2025-04-09, no purchase in your files counts "
                         "against it\n"
                         "    sale 2025-06-10: 6.0000 units, 6.0000 at a loss, 6.0000 open; "
                         "window 2025-05-11 to 2025-07-10, no purchase in your files counts "
                         "against it\n"));
    EXPECT_TRUE(
        contains(result.mOut, "  XX0000000001 Synthetic Share: listed, 2 sales, 10.0000 units\n"));
}

TEST(CliReportIntegrationTest, ASplitRatioAnsweredClosesTheQuestion) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2022-01-10T09:00:00Z,2022-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2023-01-15T09:00:00Z,2023-01-15,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,1,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,30,60,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    // The ratio 2 : 1, an empty line for the broker's date, and yes to the preview.
    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "2 : 1\n\nyes\n");

    // 1 unit before the split; 1 x 2 / 1 = 2 after it. The dates with 1 unit run from the day
    // after the purchase (2022-01-11) to the day of the sale (2025-03-10): the action comes
    // before the trades of its day, and a later date would put the sale of 2 units before the
    // split. The split is applied (a warning: the user gave the ratio) and the sale uses the
    // changed lot: the purchase row lists the 2 units after the split at EUR 10 / 2 = EUR 5.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    ASSERT_TRUE(folder.hasOutput("Doh-KDVP_2025.xml"));
    EXPECT_EQ(
        result.mOut,
        R"(Question request-0001: a corporate action of XX0000000001 Synthetic Share on 2023-01-15
  Source: Trade Republic, transaction synthetic-split-1, shares 1 (the broker's own value, not a ratio)
  You held 1.0000 units just before it.
  Enter the ratio from the announcement as new : old, for example 10 : 1 (every old share
  becomes 10 shares; "nine additional shares for every share" is also 10 : 1; a reverse
  split is 1 : 10), or your shares just before and just after it as before -> after, for
  example 0,52 -> 5,2 (a decimal comma or point), or cancel.
Ratio:
Effective date from the announcement (YYYY-MM-DD, 2022-01-11 to 2025-03-10), or empty for 2023-01-15:
After it you hold 2.0000 units (2 : 1 from 2023-01-15).
Is this right? (yes or no):

TaxBrokerReport: Doh-KDVP 2025 (original return)
Input files: 1
  source-0001 history.csv

Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2025.xml

Investments with a sale in 2025: 1
  XX0000000001 Synthetic Share: listed, 1 sale, 2.0000 units

Units past the holding period (tax free, not in the XML): 0

Private-market events skipped (only a cash amount, no units): 0

Errors: 0

Warnings: 1
  diagnostic-0001 corporate_action_applied, XX0000000001 Synthetic Share, 2023-01-15, history.csv row 3
    The corporate action was applied with the ratio 2 new for 1 old shares from 2023-01-15, as you entered it. The broker did not supply this ratio.

Answered in this run: 1
  XX0000000001 Synthetic Share, corporate action on 2023-01-15: 2 : 1 from 2023-01-15, 2.0000 units after it
)");
    EXPECT_TRUE(result.mOut.ends_with(folder.readOutput("summary_2025.txt")));

    // The purchase row of the 1 unit bought in 2022: 2 units after the split, EUR 5 each, F8 = 2;
    // the sale of 2 units at EUR 30, F8 = 0.
    const auto xml = folder.readOutput("Doh-KDVP_2025.xml");

    EXPECT_TRUE(contains(xml, R"xml(            <Purchase>
              <F1>2022-01-10</F1>
              <F2>B</F2>
              <F3>2.0000</F3>
              <F4>5.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>2.0000</F8>
)xml"));
    EXPECT_TRUE(contains(xml, R"xml(            <Sale>
              <F6>2025-03-10</F6>
              <F7>2.0000</F7>
              <F9>30.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
)xml"));
    expectValidForKdvpSchemas(xml);

    const auto fifoLists = fifoOverKdvpRows(xml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 2, then 2 - 2 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"2", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"2", "0"}));
    // 2 x 30 - 2 x 5 = 50.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"50"}));
}

TEST(CliReportIntegrationTest, ASplitRatioFromTheSharesBeforeAndAfterIsTheSameRatio) {
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
                             history},
                            "1 -> 2.00\n2023-01-15\nyes\n");

    // 1 share before and 2.00 after: 100000000 and 200000000 ticks of 10^-8, reduced to 2 : 1.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut, "After it you hold 2.0000 units (2 : 1 from 2023-01-15).\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Answered in this run: 1\n"
                         "  XX0000000001 Synthetic Share, corporate action on 2023-01-15: 2 : 1 "
                         "from 2023-01-15, 2.0000 units after it\n"));
}

TEST(CliReportIntegrationTest, AReverseSplitFromTheSharesBeforeAndAfterGivesItsRatio) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2022-01-10T09:00:00Z,2022-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2023-01-15T09:00:00Z,2023-01-15,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,1,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "10 -> 1\n\nyes\n");

    // 10 shares before, 1 after: 1 : 10, so 10 x 1 / 10 = 1 unit after it.
    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut, "  You held 10.0000 units just before it.\n"));
    EXPECT_TRUE(
        contains(result.mOut, "After it you hold 1.0000 units (1 : 10 from 2023-01-15).\n"));
}

TEST(CliReportIntegrationTest, AnUnchangedRatioIsRejectedAndAskedAgain) {
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
                             history},
                            "5 : 5\n2 : 1\n\nyes\n");

    // 5 : 5 has no preview, so it goes to the session as it is, which rejects it
    // (answer_rejected.ratio_unchanged); the question is asked again, with the same ID.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Ratio:\n"
                         "The answer to request-0001 has the same number of new and old shares, "
                         "which would not change the position. Enter the ratio from the "
                         "announcement.\n"
                         "\n"
                         "Question request-0001: a corporate action of XX0000000001 Synthetic "
                         "Share on 2023-01-15\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share, corporate action on 2023-01-15: 2 : 1 "
                         "from 2023-01-15, 2.0000 units after it\n"));
}

TEST(CliReportIntegrationTest, AZeroInARatioIsRejectedAndAskedAgain) {
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
                             history},
                            "0 : 1\n1 : 0\n2 : 1\n\nyes\n");

    // answer_rejected.ratio_not_positive names the number to correct.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "The number in newShares of the answer to request-0001 must be a whole "
                         "number above 0.\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "The number in oldShares of the answer to request-0001 must be a whole "
                         "number above 0.\n"));
    EXPECT_TRUE(contains(result.mOut, "Answered in this run: 1\n"));
}

TEST(CliReportIntegrationTest, ARatioWhosePositionDoesNotFitIsRejectedAndAskedAgain) {
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
                             history},
                            "1000000000000 : 1\n\n2 : 1\n\nyes\n");

    // 1 unit x 10^12 is more than the largest units value (about 9.2 x 10^10), so there is no
    // preview and the session rejects it (answer_rejected.ratio_out_of_range).
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "The ratio in the answer to request-0001 gives a position that cannot "
                         "be represented. Check the numbers.\n"));
    EXPECT_TRUE(contains(result.mOut, "Answered in this run: 1\n"));
}

TEST(CliReportIntegrationTest, ARatioNotConfirmedIsAskedAgain) {
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
                             history},
                            "2 : 1\n\nok\nno\n3 : 1\n\nyes\n");

    // "ok" is not an answer to the preview; no goes back to the ratio.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "After it you hold 2.0000 units (2 : 1 from 2023-01-15).\n"
                         "Is this right? (yes or no):\n"
                         "Answer yes or no.\n"
                         "Is this right? (yes or no):\n"
                         "Ratio:\n"));
    EXPECT_TRUE(contains(result.mOut, "After it you hold 3.0000 units (3 : 1 from 2023-01-15).\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "    The corporate action was applied with the ratio 3 new for 1 old "
                         "shares from 2023-01-15, as you entered it."));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share, corporate action on 2023-01-15: 3 : 1 "
                         "from 2023-01-15, 3.0000 units after it\n"));
}

TEST(CliReportIntegrationTest, AnEffectiveDateOutsideTheRangeIsRejectedAndAskedAgain) {
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
                             history},
                            "2 : 1\n15.01.2023\n2025-03-11\n2 : 1\n2025-03-10\nyes\n");

    // 15.01.2023 is not a date in the form YYYY-MM-DD. 2025-03-11 is one day after the range
    // (the sale of 2025-03-10 would come before the split), so it has no preview and the session
    // rejects it (answer_rejected.effective_date_out_of_range). 2025-03-10, the last day, is
    // accepted.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Effective date from the announcement (YYYY-MM-DD, 2022-01-11 to "
                         "2025-03-10), or empty for 2023-01-15:\n"
                         "Enter the date as YYYY-MM-DD.\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Effective date from the announcement (YYYY-MM-DD, 2022-01-11 to "
                         "2025-03-10), or empty for 2023-01-15:\n"
                         "The effective date in the answer to request-0001 must be from "
                         "2022-01-11 to 2025-03-10."));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share, corporate action on 2023-01-15: 2 : 1 "
                         "from 2025-03-10, 2.0000 units after it\n"));
}

TEST(CliReportIntegrationTest, AnUnreadableRatioIsAskedAgainWithoutAnAnswer) {
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
                             history},
                            "ten\n2.5 : 1\n-2 : 1\n+2 : 1\n: 1\n2 : one\none -> 2\n"
                            "1 -> 2.000000001\n2 : 1\n\nyes\n");

    // None of the first eight lines reaches the session, so the question is shown only once.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Ratio:\n"
                         "Enter the ratio as new : old, the shares as before -> after, or "
                         "cancel.\n"
                         "Ratio:\n"
                         "Enter whole numbers, for example 10 : 1.\n"
                         "Ratio:\n"
                         "Enter whole numbers, for example 10 : 1.\n"
                         "Ratio:\n"
                         "Enter whole numbers, for example 10 : 1.\n"
                         "Ratio:\n"
                         "Enter whole numbers, for example 10 : 1.\n"
                         "Ratio:\n"
                         "Enter whole numbers, for example 10 : 1.\n"
                         "Ratio:\n"
                         "Enter the shares as numbers above 0 with up to 8 decimals, for example "
                         "0,52 -> 5,2.\n"
                         "Ratio:\n"
                         "Enter the shares as numbers above 0 with up to 8 decimals, for example "
                         "0,52 -> 5,2.\n"
                         "Ratio:\n"
                         "Effective date"));
    EXPECT_EQ(result.mOut.find("Question request-0001"),
              result.mOut.rfind("Question request-0001"));
    EXPECT_TRUE(contains(result.mOut, "Answered in this run: 1\n"));
}

TEST(CliReportIntegrationTest, ShareCountsOfZeroOrBelowAreAskedAgainWithoutAnAnswer) {
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
                             history},
                            "0 -> 5\n-1 -> 2\n1 -> 0\n1 -> 2\n\nyes\n");

    // The command refuses the first three lines itself, so the session's message about
    // oldShares or newShares never appears. 1 -> 2 is then 2 : 1.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Ratio:\n"
                         "Enter the shares as numbers above 0 with up to 8 decimals, for example "
                         "0,52 -> 5,2.\n"
                         "Ratio:\n"
                         "Enter the shares as numbers above 0 with up to 8 decimals, for example "
                         "0,52 -> 5,2.\n"
                         "Ratio:\n"
                         "Enter the shares as numbers above 0 with up to 8 decimals, for example "
                         "0,52 -> 5,2.\n"
                         "Ratio:\n"
                         "Effective date"));
    EXPECT_FALSE(contains(result.mOut, "must be a whole number above 0"));
    EXPECT_EQ(result.mOut.find("Question request-0001"),
              result.mOut.rfind("Question request-0001"));
    EXPECT_TRUE(contains(result.mOut, "After it you hold 2.0000 units (2 : 1 from 2023-01-15).\n"));
}

TEST(CliReportIntegrationTest, ShareCountsWithADecimalCommaGiveTheirRatio) {
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
                             history},
                            "0,52 -> 5,2\n\nyes\n");

    // 0,52 is 0.52 and 5,2 is 5.2: 52000000 and 520000000 ticks of 10^-8, reduced to 10 : 1.
    // 1 unit before the split, so 1 x 10 / 1 = 10 units after it.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(
        contains(result.mOut, "After it you hold 10.0000 units (10 : 1 from 2023-01-15).\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Answered in this run: 1\n"
                         "  XX0000000001 Synthetic Share, corporate action on 2023-01-15: 10 : 1 "
                         "from 2023-01-15, 10.0000 units after it\n"));
}

TEST(CliReportIntegrationTest, AShareCountWithACommaAndAPointIsAskedAgain) {
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
                             history},
                            "1.000,5 -> 2\n2 : 1\n\nyes\n");

    // A comma and a point in one number could be digit grouping, which is never read.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Ratio:\n"
                         "Enter the shares as numbers above 0 with up to 8 decimals, for example "
                         "0,52 -> 5,2.\n"
                         "Ratio:\n"
                         "Effective date"));
    EXPECT_TRUE(contains(result.mOut, "After it you hold 2.0000 units (2 : 1 from 2023-01-15).\n"));
}

TEST(CliReportIntegrationTest, AnUnusualRatioIsWarnedAboutBeforeThePreview) {
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
                             history},
                            "1001 : 1\n\nyes\n");

    // 1001 is above the limit of 1000 for a reduced number; the answer is still accepted.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "This ratio looks unusual: check the numbers.\n"
                         "After it you hold 1001.0000 units (1001 : 1 from 2023-01-15).\n"));
    EXPECT_TRUE(contains(result.mOut, "Answered in this run: 1\n"));
}

TEST(CliReportIntegrationTest, ACancelledRatioQuestionBlocksTheReport) {
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
                             history},
                            "Cancel\n");

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(result.mOut,
                         "Errors: 1\n"
                         "  diagnostic-0001 corporate_action_ambiguous, XX0000000001 Synthetic "
                         "Share, 2023-01-15, history.csv row 3\n"
                         "    The question about the ratio of this corporate action was closed "
                         "without an answer, so the sales of this investment cannot be reported. "
                         "No Doh-KDVP file was written.\n"));
    EXPECT_FALSE(contains(result.mOut, "Open questions"));
    EXPECT_TRUE(contains(result.mOut,
                         "Answered in this run: 1\n"
                         "  XX0000000001 Synthetic Share, corporate action on 2023-01-15: "
                         "cancelled\n"));
}

TEST(CliReportIntegrationTest, ASplitWithNoInputBlocksAndListsTheQuestion) {
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
                             "--no-input",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "2 : 1\n\nyes\n");

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(contains(result.mOut, "Question"));
    EXPECT_TRUE(contains(result.mOut,
                         "Warnings: 0\n"
                         "\n"
                         "Open questions: 1\n"
                         "  request-0001 corporate_action_ratio_required, XX0000000001 Synthetic "
                         "Share\n"
                         "  Run the command again in a terminal (without --no-input) to answer "
                         "them.\n"));
    EXPECT_EQ(folder.readOutput("summary_2025.txt"), result.mOut);
}

TEST(CliReportIntegrationTest, AnInputEndingInTheMiddleOfARatioLeavesTheQuestionOpen) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2022-01-10T09:00:00Z,2022-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2023-01-15T09:00:00Z,2023-01-15,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,1,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,30,60,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    // Each run ends at another step: after the ratio, after the date, before the preview answer.
    const auto afterRatio = run({"report",
                                 "--year",
                                 "2025",
                                 "--taxpayer",
                                 taxpayer,
                                 "--out",
                                 folder.outputDirectory(),
                                 history},
                                "2 : 1\n");
    const auto afterDate = run({"report",
                                "--year",
                                "2025",
                                "--taxpayer",
                                taxpayer,
                                "--out",
                                folder.outputDirectory(),
                                history},
                               "2 : 1\n\n");
    const auto beforeAnything = run({"report",
                                     "--year",
                                     "2025",
                                     "--taxpayer",
                                     taxpayer,
                                     "--out",
                                     folder.outputDirectory(),
                                     history},
                                    "");

    EXPECT_EQ(afterRatio.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(afterRatio.mOut,
                         "2023-01-15:\n"
                         "Input ended: the open questions are listed in the summary.\n"));
    EXPECT_TRUE(contains(afterRatio.mOut, "Open questions: 1\n"));

    EXPECT_EQ(afterDate.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(afterDate.mOut,
                         "Is this right? (yes or no):\n"
                         "Input ended: the open questions are listed in the summary.\n"));
    EXPECT_TRUE(contains(afterDate.mOut, "Open questions: 1\n"));

    EXPECT_EQ(beforeAnything.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(beforeAnything.mOut,
                         "Ratio:\n"
                         "Input ended: the open questions are listed in the summary.\n"));
    EXPECT_TRUE(contains(beforeAnything.mOut, "Open questions: 1\n"));
}

TEST(CliReportIntegrationTest, TwoQuestionsAreAskedInRequestOrder) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2022-01-10T09:00:00Z,2022-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Fund,XX0000000002,1,10,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2023-01-15T09:00:00Z,2023-01-15,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Fund,XX0000000002,1,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-03-20T09:00:00Z,2025-03-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Fund,XX0000000002,-2,30,60,,,EUR,,,,,synthetic-sell-2,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "2 : 1\n\nyes\nno\n");

    // Requests are in source order: the split (row 3) before the loss sale (row 5). After the
    // first answer the run is calculated again, so the loss question is then request-0001. Both
    // answers are given, so both investments are listed.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Question request-0001: a corporate action of XX0000000002 Synthetic "
                         "Fund on 2023-01-15\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Is this right? (yes or no):\n"
                         "\n"
                         "Question request-0001: the 30-day loss rule for XX0000000001 "
                         "Synthetic Share\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Investments with a sale in 2025: 2\n"
                         "  XX0000000001 Synthetic Share: listed, 1 sale, 10.0000 units\n"
                         "  XX0000000002 Synthetic Fund: listed, 1 sale, 2.0000 units\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "Answered in this run: 2\n"
                         "  XX0000000002 Synthetic Fund, corporate action on 2023-01-15: 2 : 1 "
                         "from 2023-01-15, 2.0000 units after it\n"
                         "  XX0000000001 Synthetic Share, 30-day loss rule: no, not bought or "
                         "sold anywhere else (the open loss is deductible)\n"));
}

TEST(CliReportIntegrationTest, NoAnswerIsSavedAndTheNextRunAsksAgain) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto answered = run({"report",
                               "--year",
                               "2025",
                               "--taxpayer",
                               taxpayer,
                               "--out",
                               folder.outputDirectory(),
                               history},
                              "no\n");

    // Only the two output files, and the input folder has only what the test wrote.
    EXPECT_EQ(answered.mExitCode, kExitSuccess);
    EXPECT_EQ(folder.fileNames("out"),
              (std::vector<std::string>{"Doh-KDVP_2025.xml", "summary_2025.txt"}));
    EXPECT_EQ(folder.fileNames(""),
              (std::vector<std::string>{"history.csv", "out", "taxpayer.json"}));

    const auto again = run({"report",
                            "--year",
                            "2025",
                            "--taxpayer",
                            taxpayer,
                            "--no-input",
                            "--out",
                            folder.outputDirectory(),
                            history});

    // The second run knows nothing of the first answer: the question is open again, the report
    // is blocked and the XML of the first run is deleted.
    EXPECT_EQ(again.mExitCode, kExitBlocked);
    EXPECT_TRUE(contains(again.mOut,
                         "Open questions: 1\n"
                         "  request-0001 loss_eligibility_confirmation_required, XX0000000001 "
                         "Synthetic Share\n"));
    EXPECT_FALSE(contains(again.mOut, "Answered in this run"));
    EXPECT_EQ(folder.fileNames("out"), (std::vector<std::string>{"summary_2025.txt"}));
}

TEST(CliReportIntegrationTest, ARunWithoutQuestionsAsksNothing) {
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2025",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "no\n");

    // A gain: 10 x 20 - 10 x 10 = +100, so no question, and the output is only the summary.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_EQ(folder.readOutput("summary_2025.txt"), result.mOut);
    EXPECT_FALSE(contains(result.mOut, "Question"));
    EXPECT_FALSE(contains(result.mOut, "Answered in this run"));
    EXPECT_FALSE(contains(result.mOut, "Open questions"));
}

TEST(CliReportIntegrationTest, ProfitableSaleWithABlockedLossMakingPortionIsTwoSaleRows) {
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
                             history},
                            "yes\n");

    // Portions: 30 - 10 = +20 and 30 - 40 = -10. The sale gains 10 overall. The sale empties the
    // holding, so the purchase of 2025-02-10 is no replacement (FURS example 14) and the loss
    // unit is open. The answer yes blocks it and not the profitable unit: two rows, the
    // profitable unit first.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2025.xml\n"));
    EXPECT_TRUE(contains(result.mOut, "Answered in this run: 1\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: listed, 1 sale, 2.0000 units, 1.0000 "
                         "units with a loss blocked by the 30-day rule (F10 false)\n"));

    const auto xml = folder.readOutput("Doh-KDVP_2025.xml");

    EXPECT_TRUE(contains(xml, R"xml(            <Sale>
              <F6>2025-03-10</F6>
              <F7>1.0000</F7>
              <F9>30.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>1.0000</F8>
          </Row>
          <Row>
            <ID>3</ID>
            <Sale>
              <F6>2025-03-10</F6>
              <F7>1.0000</F7>
              <F9>30.0000</F9>
              <F10>false</F10>
            </Sale>
            <F8>0.0000</F8>
)xml"));
    expectValidForKdvpSchemas(xml);

    const auto fifoLists = fifoOverKdvpRows(xml);

    ASSERT_EQ(fifoLists.size(), 1U);

    const auto& fifoList = fifoLists.at(0);

    EXPECT_TRUE(fifoList.mProblems.empty());
    // F8: 1, 1 + 1 = 2, 2 - 1 = 1, 1 - 1 = 0.
    EXPECT_EQ(fifoList.mWrittenF8, (std::vector<std::string>{"1", "2", "1", "0"}));
    EXPECT_EQ(fifoList.mCountedF8, (std::vector<std::string>{"1", "2", "1", "0"}));
    // 1 x (30 - 10) = 20 and 1 x (30 - 40) = -10.
    EXPECT_EQ(fifoList.mSaleGains, (std::vector<std::string>{"20", "-10"}));
}

TEST(CliReportIntegrationTest, AnAdjustedUnitValueThatDoesNotFitBlocksTheReport) {
    // 1 unit bought at EUR 92,233,720,368 (just below the largest UnitPrice) and a 1 : 2 reverse
    // split: 0.5 units at EUR 184,467,440,736 each, which F4 cannot hold. The sale is a loss
    // against that value, so the loss question is also asked; the answer no changes nothing.
    const TemporaryCliFolder folder;
    const auto taxpayer = folder.writeText("taxpayer.json", kTaxpayer);
    const auto history = folder.writeCsv(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,92233720368,-92233720368,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,1,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-0.5,92233720368,46116860184,,,EUR,,,,,synthetic-sell-1,,,,)",
        });

    const auto result = run({"report",
                             "--year",
                             "2024",
                             "--taxpayer",
                             taxpayer,
                             "--out",
                             folder.outputDirectory(),
                             history},
                            "1 : 2\n\nyes\nno\n");

    EXPECT_EQ(result.mExitCode, kExitBlocked);
    EXPECT_FALSE(folder.hasOutput("Doh-KDVP_2024.xml"));
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: blocked "
                         "(corporate_action_unrepresentable)\n"
                         "    sale 2024-04-02, history.csv row 4\n"));
    EXPECT_TRUE(contains(result.mOut,
                         "corporate_action_unrepresentable, XX0000000001 Synthetic Share, "
                         "2024-04-02, history.csv row 4\n"
                         "    After a split or reverse split the value of one unit of this "
                         "investment is too large for a Doh-KDVP row, so the sales of this "
                         "investment cannot be reported. No Doh-KDVP file was written.\n"));
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

TEST(CliReportIntegrationTest, TwoPartlyBlockedSalesAreWrittenInFourSaleRows) {
    // Lots in FIFO order: 2 @ 10, 3 @ 40, 5 @ 10. The sale of 4 @ 20 uses 2 @ 10 (+20) and 2 @ 40
    // (-40); the sale of 6 @ 25 uses 1 @ 40 (-15) and 5 @ 10 (+75). No purchase lies within 30
    // days of a sale, so one question covers both sales; the answer yes blocks the loss units and
    // not the profitable ones: each sale is written as two rows.
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
                             history},
                            "yes\n");

    // 2 + 2 + 1 + 5 = 10 listed units, of which 2 + 1 = 3 have a blocked loss.
    EXPECT_EQ(result.mExitCode, kExitSuccess);
    EXPECT_TRUE(contains(result.mOut,
                         "  XX0000000001 Synthetic Share: listed, 2 sales, 10.0000 units, 3.0000 "
                         "units with a loss blocked by the 30-day rule (F10 false)\n"));
    EXPECT_TRUE(result.mOut.ends_with(folder.readOutput("summary_2024.txt")));

    // Rows: the three purchases (F8 2, 5, 10), then sale 1 as 2 units with F10 true (F8 8) and
    // 2 units with F10 false (F8 6), then sale 2 as 1 unit with F10 false (F8 5) and 5 units with
    // F10 true (F8 0).
    const auto xml = folder.readOutput("Doh-KDVP_2024.xml");

    EXPECT_TRUE(contains(xml, R"xml(            <Sale>
              <F6>2024-03-10</F6>
              <F7>2.0000</F7>
              <F9>20.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>8.0000</F8>
          </Row>
          <Row>
            <ID>4</ID>
            <Sale>
              <F6>2024-03-10</F6>
              <F7>2.0000</F7>
              <F9>20.0000</F9>
              <F10>false</F10>
            </Sale>
            <F8>6.0000</F8>
          </Row>
          <Row>
            <ID>5</ID>
            <Sale>
              <F6>2024-05-10</F6>
              <F7>1.0000</F7>
              <F9>25.0000</F9>
              <F10>false</F10>
            </Sale>
            <F8>5.0000</F8>
          </Row>
          <Row>
            <ID>6</ID>
            <Sale>
              <F6>2024-05-10</F6>
              <F7>5.0000</F7>
              <F9>25.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
)xml"));
    expectValidForKdvpSchemas(xml);
}

TEST(CliReportIntegrationTest, HelpExplainsTheCommand) {
    const auto general = run({"--help"});

    EXPECT_EQ(general.mExitCode, kExitSuccess);
    EXPECT_TRUE(general.mErr.empty());
    EXPECT_TRUE(contains(general.mOut,
                         "  taxbroker report --year YYYY --taxpayer FILE [--self-report] "
                         "[--no-input]\n"
                         "                   --out DIR CSV [CSV ...]\n"));

    const auto report = run({"report", "--help"});

    EXPECT_EQ(report.mExitCode, kExitSuccess);
    EXPECT_TRUE(report.mErr.empty());
    EXPECT_TRUE(contains(report.mOut, "  --self-report      Write a self-report (samoprijava)"));
    EXPECT_TRUE(contains(report.mOut,
                         "  --no-input         Ask no questions: an open question blocks the "
                         "report (see\n"));
    EXPECT_TRUE(contains(report.mOut,
                         "Questions:\n"
                         "  Some cases need your answer. The command asks in the terminal, one "
                         "question at a time,\n"));
    EXPECT_TRUE(contains(report.mOut,
                         "lists them. The answers are used only in this run and are never saved, "
                         "so the next run\n  asks again.\n"));
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
                         "  1  The report is blocked: the errors and the open questions are in "
                         "the summary. No XML\n"
                         "     was written.\n"));
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
    // Buy 10 @ 30, sell 10 @ 20: a loss whose question --no-input leaves open, so the year is
    // blocked.
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
                             "--no-input",
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
