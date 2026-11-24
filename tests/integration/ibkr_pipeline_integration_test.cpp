#include "../support/kdvp_row_fifo.hpp"
#include "../support/xsd_validation.hpp"
#include "parsers/ibkr_parser.hpp"
#include "parsers/reference_catalogue.hpp"
#include "processors/capital_gains_report_builder.hpp"
#include "processors/fifo_matcher.hpp"
#include "processors/income_reports.hpp"
#include "processors/loss_eligibility.hpp"
#include "processors/tax_processor.hpp"
#include "taxbroker/decision_session.hpp"
#include "utils/date_utils.hpp"
#include "xml/kdvp_xml_writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The Interactive Brokers parser is not registered in the application until the parser selection
// exists, so these tests run its files through the stages that follow it: the merger, the
// reporting year, FIFO matching, Doh-KDVP and the income models. All inputs are synthetic.

namespace {
using namespace taxbroker;
using taxbroker::test::expectValidForKdvpSchemas;

std::filesystem::path ibkrFolder() {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / "ibkr";
}

/// Parses one XML text as source 0 and merges it alone, as a run with only this file does.
StatementMergeResult mergeText(std::string_view aXml) {
    std::vector<StatementMergeInput> inputs;

    inputs.push_back({0, ibkr::IbkrParser{}.parse(aXml, "ibkr.xml", 0)});

    return DeterministicStatementMerger{}.merge(std::move(inputs));
}

TEST(IbkrPipelineIntegrationTest, APurchaseAndASaleOfAnIbkrFileWriteTheWholeDohKdvp) {
    // equivalence/ibkr.xml: FR0000000077, a purchase of 5 units at EUR 20 on 2024-03-05 and a sale
    // of 2 units at EUR 25 on 2024-06-10, each with a EUR 1 fee that the form does not use.
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = "12345679", .mIsResident = true},
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
    };
    std::vector<StatementMergeInput> inputs;

    inputs.push_back(
        {0, ibkr::IbkrParser{}.parseFile(ibkrFolder() / "equivalence" / "ibkr.xml", 0)});

    const auto prepared = TaxProcessor::prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto fifo = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(fifo.mDiagnostics.empty());
    ASSERT_EQ(fifo.mIsins.size(), 1U);

    const auto& isin = fifo.mIsins.at(0);

    EXPECT_EQ(isin.mIsin, "FR0000000077");
    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 1U);

    const auto& match = isin.mMatches.at(0);

    EXPECT_EQ(match.mUnits, 200'000'000);                                                // 2 units
    EXPECT_EQ(match.mBasis.mTicks, ExactRational{WideInteger{400'000'000'000'000'000}}); // 2 * 20
    EXPECT_EQ(match.mSaleValueTicks, ExactRational{WideInteger{500'000'000'000'000'000}}); // 2 * 25
    EXPECT_EQ(match.mGainTicks, ExactRational{WideInteger{100'000'000'000'000'000}}); // 50 - 40

    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 300'000'000); // 5 - 2 = 3 units

    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(prepared.mMerged.mStatement,
                                                    *prepared.mLedger,
                                                    fifo,
                                                    decisions);
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
    // Row 0 lists only the 2 units of the purchase that the sale uses. F8 is the running stock:
    // 2, then 2 - 2 = 0.
    EXPECT_EQ(*result.mXml, R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345679</edp:taxNumber>
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
          <ISIN>FR0000000077</ISIN>
          <Name>SYNTHETIC EXAMPLE G SA</Name>
          <IsFond>false</IsFond>
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-03-05</F1>
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
              <F6>2024-06-10</F6>
              <F7>2.0000</F7>
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

TEST(IbkrPipelineIntegrationTest, ADividendWithItsTaxAndBrokerInterestBecomeIncomeRows) {
    // A EUR 10.00 dividend of FR0000000077 on 2024-04-15 with EUR 1.50 French tax on its own
    // cash row (one payment, ID A9201), and EUR 2.50 interest on the broker's cash on 2024-04-03.
    constexpr std::string_view xml = R"(<FlexQueryResponse queryName="Synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000" fromDate="20240101" toDate="20241231" period="" whenGenerated="20250301;101500">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <CashTransactions>
        <CashTransaction accountId="U0000000" transactionID="7003" actionID="A9201" levelOfDetail="DETAIL" type="Dividends" assetCategory="STK" symbol="SYNG" description="SYNG CASH DIVIDEND EUR 2.00 PER SHARE" isin="FR0000000077" currency="EUR" dateTime="20240415" settleDate="20240415" reportDate="20240415" amount="10" issuerCountryCode="FR"/>
        <CashTransaction accountId="U0000000" transactionID="7004" actionID="A9201" levelOfDetail="DETAIL" type="Withholding Tax" assetCategory="STK" symbol="SYNG" description="SYNG CASH DIVIDEND EUR 2.00 PER SHARE - FR TAX" isin="FR0000000077" currency="EUR" dateTime="20240415" settleDate="20240415" reportDate="20240415" amount="-1.5" issuerCountryCode="FR"/>
        <CashTransaction accountId="U0000000" transactionID="7006" actionID="" levelOfDetail="DETAIL" type="Broker Interest Received" assetCategory="CASH" symbol="" description="EUR CREDIT INT FOR MAR-2024" isin="" currency="EUR" dateTime="20240403" settleDate="20240403" reportDate="20240403" amount="2.5" issuerCountryCode=""/>
      </CashTransactions>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>)";

    const auto prepared = TaxProcessor::prepareReportingYear({2024, mergeText(xml)});

    ASSERT_TRUE(prepared.mLedger.has_value());

    ApplicationResultInput input{.mReportingYear = 2024,
                                 .mMergeDiagnostics = prepared.mMerged.mDiagnostics,
                                 .mProcessingDiagnostics = prepared.mDiagnostics};
    RunDecisions decisions;
    const OfficialRateTable rates;
    const auto reports = prepareIncomeReports(input,
                                              prepared.mMerged.mStatement,
                                              *prepared.mLedger,
                                              referenceCatalogue(),
                                              rates,
                                              decisions);

    // The dividend row and its tax row are one payment: gross EUR 10.00, tax EUR 1.50, and the
    // country of the export.
    ASSERT_EQ(reports.mDividends.mRows.size(), 1U);

    const auto& dividend = reports.mDividends.mRows.at(0);

    EXPECT_EQ(dividend.mIsin, "FR0000000077");
    EXPECT_EQ(dividend.mDate, *parseCalendarDate("2024-04-15"));
    EXPECT_EQ(dividend.mGrossIncome, 100'000);            // EUR 10.00 at 4 decimals
    EXPECT_EQ(dividend.mWithholding.mForeignTax, 15'000); // EUR 1.50 at 4 decimals
    EXPECT_FALSE(dividend.mWithholding.mBlankInExport);
    EXPECT_EQ(dividend.mSourceCountry, "FR");

    // The interest of the broker's cash is one Doh-Obr row, not a Doh-DHO row.
    EXPECT_TRUE(reports.mDeposit.mRows.empty());
    ASSERT_EQ(reports.mOther.mRows.size(), 1U);

    const auto& interest = reports.mOther.mRows.at(0);

    EXPECT_EQ(interest.mDate, *parseCalendarDate("2024-04-03"));
    EXPECT_EQ(interest.mGrossInterest, 25'000); // EUR 2.50 at 4 decimals
    EXPECT_TRUE(std::holds_alternative<BrokerCashInterest>(interest.mKind));
}

TEST(IbkrPipelineIntegrationTest, TwoAccountsShareOneHoldingWithTheSplitOfEachAccount) {
    // contract_example.xml: accounts U0000000 (IB-IE) and U0000001 (IB-CE) both trade
    // US0000000010 (buys 1001 and 5001, a sale 1002) and both carry the same split S9001.
    std::vector<StatementMergeInput> inputs;

    inputs.push_back({0, ibkr::IbkrParser{}.parseFile(ibkrFolder() / "contract_example.xml", 0)});

    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));
    const auto& instruments = merged.mStatement.mPresentation.mTradeInstruments;
    const auto found = std::find_if(
        instruments.begin(),
        instruments.end(),
        [](const TradeInstrument& aInstrument) { return aInstrument.mIsin == "US0000000010"; });

    ASSERT_NE(found, instruments.end());
    EXPECT_EQ(found->mTransactions.size(), 3U);     // buys 1001 and 5001, sale 1002
    EXPECT_EQ(found->mCorporateActions.size(), 2U); // the split of each account, one pool
}

} // namespace
