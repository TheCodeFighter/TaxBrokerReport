#include "../support/kdvp_row_fifo.hpp"
#include "../support/xsd_validation.hpp"
#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/application_request.hpp"
#include "taxbroker/application_run.hpp"
#include "taxbroker/input_file.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;
using taxbroker::test::expectValidForDhoSchema;
using taxbroker::test::expectValidForDivSchema;
using taxbroker::test::expectValidForKdvpSchemas;
using taxbroker::test::expectValidForObrSchema;
using taxbroker::test::fifoOverKdvpRows;
using taxbroker::test::KdvpListFifo;

// All rows are synthetic. Every scenario runs the whole application (`ApplicationRun`) on a
// Trade Republic CSV file and an Interactive Brokers XML file together. Amounts are in EUR unless
// a test says otherwise, so no rate file is needed. Money is stored with 4 decimals (30'000 is
// EUR 3.00) and units with 8 decimals (100'000'000 is 1 unit). The synthetic ISINs use the prefix
// XX, which is never in the built-in payer file, so no payer is found.
//
// The Interactive Brokers `dateTime` is US Eastern time: `20240610;101500` is 14:15:00 UTC in
// summer (UTC-4), and `20240305;094210` is 14:42:10 UTC in winter (UTC-5).
constexpr std::string_view kTradeRepublicHeader =
    "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,"
    "currency,original_amount,original_currency,fx_rate,description,transaction_id,"
    "counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

TaxpayerMetadata syntheticTaxpayer() {
    return {.mTaxNumber = "12345679",
            .mName = "Synthetic Taxpayer",
            .mAddress = "Synthetic Street 1",
            .mCity = "Synthetic City",
            .mPostCode = "1000",
            .mPostName = "Synthetic City",
            .mBirthDate = *parseCalendarDate("1990-01-31"),
            .mIsResident = true,
            .mEmail = "taxpayer@example.com",
            .mPhoneNumber = "+38600000000"};
}

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

/// A Trade Republic file made of the header and the given rows.
BrokerInputFile tradeRepublicFile(std::string_view aName,
                                  const std::vector<std::string_view>& aRows) {
    std::string content{kTradeRepublicHeader};

    for (const std::string_view row : aRows)
    {
        content += row;
        content += '\n';
    }

    return {.mBroker = Broker::TradeRepublic,
            .mFile = {.mName = std::string{aName}, .mContent = std::move(content)}};
}

/// An Interactive Brokers file with the given XML text, written in full by the test.
BrokerInputFile ibkrFile(std::string_view aName, std::string_view aXml) {
    return {.mBroker = Broker::InteractiveBrokers,
            .mFile = {.mName = std::string{aName}, .mContent = std::string{aXml}}};
}

/// A request for the year with the synthetic taxpayer and the given files, in this order.
ApplicationRequest requestOf(int aYear, std::vector<BrokerInputFile> aFiles) {
    return {.mReportingYear = aYear,
            .mFilingType = FilingType::Original,
            .mTaxpayer = syntheticTaxpayer(),
            .mBrokerFiles = std::move(aFiles)};
}

/// The run, or an empty optional (and a failed test) when the files were not accepted.
std::optional<ApplicationRun> startRun(ApplicationRequest aRequest) {
    std::variant<ApplicationInputFault, ApplicationRun> started =
        ApplicationRun::start(std::move(aRequest));
    ApplicationRun* run = std::get_if<ApplicationRun>(&started);

    EXPECT_NE(run, nullptr);

    if (run == nullptr)
    {
        return std::nullopt;
    }

    return std::optional<ApplicationRun>{std::move(*run)};
}

/// Answers the request of the given ID at the current revision of the run.
std::vector<RejectedAnswer>
answerRequest(ApplicationRun& aRun, const std::string& aRequestId, AnswerValue aValue) {
    return aRun.session().answer({{aRequestId, aRun.result().mRunRevision, std::move(aValue)}});
}

bool contains(const std::string& aText, std::string_view aPart) {
    return aText.find(aPart) != std::string::npos;
}

/// The diagnostics of the result with this message key, in result order.
std::vector<const ApplicationDiagnostic*> diagnosticsWithKey(const ApplicationResult& aResult,
                                                             MessageKey aKey) {
    std::vector<const ApplicationDiagnostic*> found;

    for (const ApplicationDiagnostic& diagnostic : aResult.mDiagnostics)
    {
        if (diagnostic.mMessageKey == aKey)
        {
            found.push_back(&diagnostic);
        }
    }

    return found;
}

/// The loss question data of the request at aIndex.
const LossEligibilityQuestion& lossQuestionAt(const ApplicationResult& aResult,
                                              std::size_t aIndex) {
    return std::get<LossEligibilityQuestion>(aResult.mRequests.at(aIndex).mData);
}

// fifo.md, "Cross-broker example", with prices: the purchases are at Trade Republic and the sales
// at Interactive Brokers, and the sale of 2023 uses units of the first purchase.
TEST(MixedBrokerIntegrationTest, TheCrossBrokerExampleOfFifoMdWritesTheDohKdvpOf2024) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2024,
        {tradeRepublicFile(
             "tr.csv",
             {
                 R"(2021-02-10T09:00:00Z,2021-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1.5,10,-15,,,EUR,,,,,synthetic-buy-1,,,,)",
                 R"(2023-09-10T09:00:00Z,2023-09-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.75,11,-8.25,,,EUR,,,,,synthetic-buy-2,,,,)",
             }),
         ibkrFile("ibkr.xml", R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades>
        <Trade transactionID="ib-sell-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20230405" dateTime="20230405;040000" buySell="SELL" quantity="-0.4" tradePrice="12" proceeds="4.8" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
        <Trade transactionID="ib-sell-2" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20240603" dateTime="20240603;040000" buySell="SELL" quantity="-1.6" tradePrice="14" proceeds="22.4" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
      </Trades>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)xml")}));

    ASSERT_TRUE(run.has_value());

    const ApplicationResult& result = run->result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);

    ASSERT_TRUE(result.report(ReportType::CapitalGains).mXml.has_value());

    const std::string& xml = *result.report(ReportType::CapitalGains).mXml;

    expectValidForKdvpSchemas(xml);

    // The sale of 2023 took 0.4 of the 1.5 units of the first purchase, so that row lists 1.1
    // units (1.5 - 0.4) at EUR 10. The 2024 sale of 1.6 units takes those 1.1 units and then 0.5
    // of the 0.75 units bought on 2023-09-10 at EUR 11. The 2023 sale has no row.
    EXPECT_TRUE(contains(xml, "<F1>2021-02-10</F1>"));
    EXPECT_TRUE(contains(xml, "<F3>1.1000</F3>"));
    EXPECT_TRUE(contains(xml, "<F4>10.0000</F4>"));
    EXPECT_TRUE(contains(xml, "<F1>2023-09-10</F1>"));
    EXPECT_TRUE(contains(xml, "<F3>0.5000</F3>"));
    EXPECT_TRUE(contains(xml, "<F4>11.0000</F4>"));
    EXPECT_TRUE(contains(xml, "<F6>2024-06-03</F6>"));
    EXPECT_TRUE(contains(xml, "<F7>1.6000</F7>"));
    EXPECT_TRUE(contains(xml, "<F9>14.0000</F9>"));
    EXPECT_TRUE(contains(xml, "<F10>true</F10>"));
    EXPECT_FALSE(contains(xml, "<F6>2023-04-05</F6>"));

    const std::vector<KdvpListFifo> lists = fifoOverKdvpRows(xml);

    ASSERT_EQ(lists.size(), 1U);
    EXPECT_TRUE(lists[0].mProblems.empty());
    // F8: 1.1, 1.1 + 0.5 = 1.6, 1.6 - 1.6 = 0.
    EXPECT_EQ(lists[0].mWrittenF8, (std::vector<std::string>{"1.1", "1.6", "0"}));
    // Gain: 1.6 x 14 - (1.1 x 10 + 0.5 x 11) = 22.40 - 16.50 = 5.90.
    EXPECT_EQ(lists[0].mSaleGains, (std::vector<std::string>{"5.9"}));
}

// The same files in the other order, and a repeated run, give the same documents. The values are
// those of the first scenario; this test only shows that the order does not matter.
TEST(MixedBrokerIntegrationTest, EitherFileOrderAndARepeatedRunGiveTheSameForms) {
    const BrokerInputFile tradeRepublic = tradeRepublicFile(
        "tr.csv",
        {
            R"(2021-02-10T09:00:00Z,2021-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1.5,10,-15,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2023-09-10T09:00:00Z,2023-09-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.75,11,-8.25,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-05-02T10:00:00Z,2024-05-02,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,2.00,,,EUR,,,,Synthetic bank interest,synthetic-interest-1,,,,)",
        });
    const BrokerInputFile ibkr =
        ibkrFile("ibkr.xml", R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades>
        <Trade transactionID="ib-sell-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20230405" dateTime="20230405;040000" buySell="SELL" quantity="-0.4" tradePrice="12" proceeds="4.8" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
        <Trade transactionID="ib-sell-2" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20240603" dateTime="20240603;040000" buySell="SELL" quantity="-1.6" tradePrice="14" proceeds="22.4" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
      </Trades>
      <CashTransactions>
        <CashTransaction transactionID="ib-dividend-1" actionID="A1" levelOfDetail="DETAIL" type="Dividends" assetCategory="STK" symbol="SYNB" description="SYNB CASH DIVIDEND EUR 10.00" isin="XX0000000002" currency="EUR" dateTime="20240415" settleDate="20240415" reportDate="20240415" amount="10" issuerCountryCode=""/>
      </CashTransactions>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)xml");

    std::optional<ApplicationRun> first = startRun(requestOf(2024, {tradeRepublic, ibkr}));
    std::optional<ApplicationRun> swapped = startRun(requestOf(2024, {ibkr, tradeRepublic}));
    std::optional<ApplicationRun> repeated = startRun(requestOf(2024, {tradeRepublic, ibkr}));

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(swapped.has_value());
    ASSERT_TRUE(repeated.has_value());

    // Doh-KDVP, Doh-Div (EUR 10.00) and Doh-DHO (EUR 2.00) are made; Doh-Obr has no data.
    const ApplicationResult& result = first->result();

    // The deposit interest opens the one question about the allowance; the forms are made as for
    // yes while it is open.
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::DepositInterestTotalRequired);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);
    ASSERT_TRUE(result.report(ReportType::CapitalGains).mXml.has_value());
    ASSERT_TRUE(result.report(ReportType::Dividends).mXml.has_value());
    ASSERT_TRUE(result.report(ReportType::BankDepositInterest).mXml.has_value());
    expectValidForKdvpSchemas(*result.report(ReportType::CapitalGains).mXml);
    expectValidForDivSchema(*result.report(ReportType::Dividends).mXml);
    expectValidForDhoSchema(*result.report(ReportType::BankDepositInterest).mXml);

    for (const ApplicationRun* other : {&*swapped, &*repeated})
    {
        const ApplicationResult& again = other->result();

        EXPECT_EQ(again.mStatus, result.mStatus);

        for (const ReportType type : {ReportType::CapitalGains,
                                      ReportType::Dividends,
                                      ReportType::BankDepositInterest,
                                      ReportType::OtherInterest})
        {
            EXPECT_EQ(again.report(type).mStatus, result.report(type).mStatus);
            EXPECT_EQ(again.report(type).mXml, result.report(type).mXml);
        }
    }

    // The same order twice gives the same JSON. (In the other order the files have other source
    // indexes.)
    EXPECT_EQ(api::serializeApplicationResultJson(first->result()),
              api::serializeApplicationResultJson(repeated->result()));
}

// An ID is unique per broker, not across brokers.
TEST(MixedBrokerIntegrationTest, OneTransactionIdAtBothBrokersIsTwoTrades) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2024,
        {tradeRepublicFile(
             "tr.csv",
             {
                 R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,7001,,,,)",
             }),
         ibkrFile("ibkr.xml", R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades>
        <Trade transactionID="7001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20240202" dateTime="20240202;040000" buySell="BUY" quantity="5" tradePrice="12" proceeds="-60" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
        <Trade transactionID="7002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20240502" dateTime="20240502;040000" buySell="SELL" quantity="-15" tradePrice="13" proceeds="195" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
      </Trades>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)xml")}));

    ASSERT_TRUE(run.has_value());

    const ApplicationResult& result = run->result();

    // A shared ID would reject the group as a conflict and block the sale.
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);

    ASSERT_TRUE(result.report(ReportType::CapitalGains).mXml.has_value());

    const std::string& xml = *result.report(ReportType::CapitalGains).mXml;

    expectValidForKdvpSchemas(xml);

    // Purchases of 10 units at EUR 10 and 5 units at EUR 12, and one sale of 15 units at EUR 13.
    EXPECT_TRUE(contains(xml, "<F1>2024-02-01</F1>"));
    EXPECT_TRUE(contains(xml, "<F4>10.0000</F4>"));
    EXPECT_TRUE(contains(xml, "<F1>2024-02-02</F1>"));
    EXPECT_TRUE(contains(xml, "<F4>12.0000</F4>"));
    EXPECT_TRUE(contains(xml, "<F7>15.0000</F7>"));
    EXPECT_TRUE(contains(xml, "<F9>13.0000</F9>"));

    const std::vector<KdvpListFifo> lists = fifoOverKdvpRows(xml);

    ASSERT_EQ(lists.size(), 1U);
    EXPECT_TRUE(lists[0].mProblems.empty());
    // F8: 10, 10 + 5 = 15, 15 - 15 = 0. Gain: 15 x 13 - (10 x 10 + 5 x 12) = 195 - 160 = 35.
    EXPECT_EQ(lists[0].mWrittenF8, (std::vector<std::string>{"10", "15", "0"}));
    EXPECT_EQ(lists[0].mSaleGains, (std::vector<std::string>{"35"}));
}

// The trades of one day at two brokers are ordered by their time in UTC, not by the broker.
TEST(MixedBrokerIntegrationTest, AnIbkrSaleAfterATradeRepublicPurchaseOnOneDayIsMatched) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2024,
        {tradeRepublicFile(
             "tr.csv",
             {
                 // 14:00:00Z, before the sale below.
                 R"(2024-06-10T14:00:00Z,2024-06-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
             }),
         ibkrFile("ibkr.xml", R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades>
        <Trade transactionID="ib-sell-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20240610" dateTime="20240610;101500" buySell="SELL" quantity="-10" tradePrice="12" proceeds="120" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
      </Trades>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)xml")}));

    ASSERT_TRUE(run.has_value());

    const ApplicationResult& result = run->result();

    // 10:15 US Eastern in June is 14:15:00Z, after the purchase at 14:00:00Z.
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);

    ASSERT_TRUE(result.report(ReportType::CapitalGains).mXml.has_value());

    const std::string& xml = *result.report(ReportType::CapitalGains).mXml;

    expectValidForKdvpSchemas(xml);

    const std::vector<KdvpListFifo> lists = fifoOverKdvpRows(xml);

    ASSERT_EQ(lists.size(), 1U);
    EXPECT_TRUE(lists[0].mProblems.empty());
    // Gain: 10 x 12 - 10 x 10 = EUR 20.
    EXPECT_EQ(lists[0].mSaleGains, (std::vector<std::string>{"20"}));
}

TEST(MixedBrokerIntegrationTest,
     AnIbkrSaleBeforeATradeRepublicPurchaseOnOneDayIsIncompleteHistory) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2024,
        {ibkrFile("ibkr.xml", R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades>
        <Trade transactionID="ib-sell-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20240610" dateTime="20240610;101500" buySell="SELL" quantity="-10" tradePrice="12" proceeds="120" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
      </Trades>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)xml"),
         tradeRepublicFile(
             "tr.csv",
             {
                 // 14:30:00Z, after the sale at 14:15:00Z: the sale has no purchase before it.
                 R"(2024-06-10T14:30:00Z,2024-06-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
                 R"(2024-07-01T10:00:00Z,2024-07-01,DEFAULT,CASH,DIVIDEND,STOCK,Other Share,XX0000000002,,,5,,,EUR,,,,,synthetic-dividend-1,,,,)",
             })}));

    ASSERT_TRUE(run.has_value());

    const ApplicationResult& result = run->result();

    // One error for XX0000000001, from the Interactive Brokers file (source index 0).
    const std::vector<const ApplicationDiagnostic*> incomplete =
        diagnosticsWithKey(result, MessageKey::IncompleteHistory);

    ASSERT_EQ(incomplete.size(), 1U);
    EXPECT_EQ(incomplete[0]->mIsin, "XX0000000001");
    ASSERT_EQ(incomplete[0]->mSources.size(), 1U);
    EXPECT_EQ(incomplete[0]->mSources[0].mBroker, Broker::InteractiveBrokers);
    EXPECT_EQ(incomplete[0]->mSources[0].mSourceIndex, 0U);

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::CapitalGainsFailureResolutionRequired);

    // The dividend of another share goes on: Doh-Div has its one row of EUR 5.00.
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Generated);
    ASSERT_TRUE(result.report(ReportType::Dividends).mIncome.has_value());
    ASSERT_EQ(result.report(ReportType::Dividends).mIncome->mRows.size(), 1U);
    EXPECT_EQ(result.report(ReportType::Dividends).mIncome->mRows[0].mGross, 50'000); // EUR 5.00
}

// architecture.md: a split of the day comes before the trades of its ISIN, whatever the times.
TEST(MixedBrokerIntegrationTest, ASameDaySplitOfOneBrokerComesBeforeTheSaleAtTheOther) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2024,
        {tradeRepublicFile(
             "tr.csv",
             {
                 R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
                 // 18:00:00Z, after the sale at 14:15:00Z.
                 R"(2024-06-10T18:00:00Z,2024-06-10,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,10,,,,,,,,,,synthetic-split-1,,,,)",
             }),
         ibkrFile("ibkr.xml", R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades>
        <Trade transactionID="ib-buy-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20240304" dateTime="20240304;040000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
        <Trade transactionID="ib-sell-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20240610" dateTime="20240610;101500" buySell="SELL" quantity="-30" tradePrice="12" proceeds="360" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
      </Trades>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)xml")}));

    ASSERT_TRUE(run.has_value());

    const ApplicationResult& asked = run->result();

    ASSERT_EQ(asked.mRequests.size(), 1U);
    EXPECT_EQ(asked.mRequests[0].mType, RequestType::CorporateActionRatioRequired);
    EXPECT_EQ(asked.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);

    // 10 + 5 = 15 units are held before the split; 2 : 1 makes 30 units.
    EXPECT_TRUE(answerRequest(*run,
                              asked.mRequests[0].mId,
                              SplitRatioAnswer{.mNewShares = 2,
                                               .mOldShares = 1,
                                               .mEffectiveDate = day("2024-06-10"),
                                               .mConfirmedUnits = 3'000'000'000})
                    .empty());

    const ApplicationResult& result = run->result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);

    // One file has the split row, so there is no warning about rows from several files.
    EXPECT_TRUE(
        diagnosticsWithKey(result, MessageKey::CorporateActionSourcesMismatchPossible).empty());

    ASSERT_TRUE(result.report(ReportType::CapitalGains).mXml.has_value());

    const std::string& xml = *result.report(ReportType::CapitalGains).mXml;

    expectValidForKdvpSchemas(xml);

    // The 10 units of EUR 10 are 20 units at EUR 5; the 5 units of EUR 20 are 10 units at EUR 10.
    // Both rows keep their purchase dates.
    EXPECT_TRUE(contains(xml, "<F1>2024-03-01</F1>"));
    EXPECT_TRUE(contains(xml, "<F1>2024-03-04</F1>"));
    EXPECT_TRUE(contains(xml, "<F3>20.0000</F3>"));
    EXPECT_TRUE(contains(xml, "<F4>5.0000</F4>"));
    EXPECT_TRUE(contains(xml, "<F3>10.0000</F3>"));
    EXPECT_TRUE(contains(xml, "<F4>10.0000</F4>"));
    EXPECT_TRUE(contains(xml, "<F7>30.0000</F7>"));
    EXPECT_TRUE(contains(xml, "<F10>true</F10>"));

    const std::vector<KdvpListFifo> lists = fifoOverKdvpRows(xml);

    ASSERT_EQ(lists.size(), 1U);
    EXPECT_TRUE(lists[0].mProblems.empty());
    // F8: 20, 20 + 10 = 30, 30 - 30 = 0. Gain: 30 x 12 - (20 x 5 + 10 x 10) = 360 - 200 = 160.
    EXPECT_EQ(lists[0].mWrittenF8, (std::vector<std::string>{"20", "30", "0"}));
    EXPECT_EQ(lists[0].mSaleGains, (std::vector<std::string>{"160"}));
}

// tax_rules.md: a purchase within 30 days after a loss sale replaces its units, at any broker.
TEST(MixedBrokerIntegrationTest, ADecemberLossAtOneBrokerIsBlockedByAJanuaryPurchaseAtTheOther) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2024,
        {tradeRepublicFile(
             "tr.csv",
             {
                 R"(2023-09-01T09:00:00Z,2023-09-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,10,-1000,,,EUR,,,,,synthetic-buy-1,,,,)",
                 R"(2024-12-20T09:00:00Z,2024-12-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,8,32,,,EUR,,,,,synthetic-sell-1,,,,)",
             }),
         ibkrFile("ibkr.xml", R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades>
        <Trade transactionID="ib-buy-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20250106" dateTime="20250106;040000" buySell="BUY" quantity="4" tradePrice="8" proceeds="-32" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
      </Trades>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)xml")}));

    ASSERT_TRUE(run.has_value());

    const ApplicationResult& result = run->result();

    // All 4 units are replaced by the purchase of 2025-01-06 (17 days later), so nothing is asked.
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);

    ASSERT_TRUE(result.report(ReportType::CapitalGains).mXml.has_value());

    const std::string& xml = *result.report(ReportType::CapitalGains).mXml;

    expectValidForKdvpSchemas(xml);
    EXPECT_TRUE(contains(xml, "<F10>false</F10>"));
    EXPECT_FALSE(contains(xml, "<F1>2025-01-06</F1>"));

    const std::vector<KdvpListFifo> lists = fifoOverKdvpRows(xml);

    ASSERT_EQ(lists.size(), 1U);
    EXPECT_TRUE(lists[0].mProblems.empty());
    // F8: 4 units listed, 4 - 4 = 0. Gain: 4 x 8 - 4 x 10 = -8.
    EXPECT_EQ(lists[0].mWrittenF8, (std::vector<std::string>{"4", "0"}));
    EXPECT_EQ(lists[0].mSaleGains, (std::vector<std::string>{"-8"}));
}

// A replacement purchase is used once: the one of January is taken by the loss of December.
TEST(MixedBrokerIntegrationTest, AJanuaryReplacementUsedForTheYearBeforeIsNotUsedAgainAtTheOther) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2024,
        {tradeRepublicFile(
             "tr.csv",
             {
                 R"(2023-06-01T09:00:00Z,2023-06-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,10,-1000,,,EUR,,,,,synthetic-buy-1,,,,)",
                 R"(2024-01-05T09:00:00Z,2024-01-05,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,8,-80,,,EUR,,,,,synthetic-buy-2,,,,)",
             }),
         ibkrFile("ibkr.xml", R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades>
        <Trade transactionID="ib-sell-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20231220" dateTime="20231220;040000" buySell="SELL" quantity="-10" tradePrice="8" proceeds="80" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
        <Trade transactionID="ib-sell-2" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20240110" dateTime="20240110;040000" buySell="SELL" quantity="-10" tradePrice="7" proceeds="70" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
      </Trades>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)xml")}));

    ASSERT_TRUE(run.has_value());

    const ApplicationResult& asked = run->result();

    // The purchase of 2024-01-05 replaces the 10 units sold at a loss on 2023-12-20 (16 days
    // before it), so it is not a replacement for the loss of 2024-01-10: 10 units stay open.
    ASSERT_EQ(asked.mRequests.size(), 1U);
    EXPECT_EQ(asked.mRequests[0].mType, RequestType::LossEligibilityConfirmationRequired);
    ASSERT_EQ(lossQuestionAt(asked, 0).mSales.size(), 1U);
    EXPECT_EQ(lossQuestionAt(asked, 0).mSales[0].mSaleDate, day("2024-01-10"));
    EXPECT_EQ(lossQuestionAt(asked, 0).mSales[0].mKnownIneligibleUnits, 0);
    EXPECT_EQ(lossQuestionAt(asked, 0).mSales[0].mOpenUnits, 1'000'000'000); // 10 units

    EXPECT_TRUE(answerRequest(*run, asked.mRequests[0].mId, NoOtherActivityAnswer{}).empty());

    const ApplicationResult& result = run->result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);

    ASSERT_TRUE(result.report(ReportType::CapitalGains).mXml.has_value());

    const std::string& xml = *result.report(ReportType::CapitalGains).mXml;

    expectValidForKdvpSchemas(xml);
    EXPECT_TRUE(contains(xml, "<F10>true</F10>"));

    const std::vector<KdvpListFifo> lists = fifoOverKdvpRows(xml);

    ASSERT_EQ(lists.size(), 1U);
    // Gain: 10 x 7 - 10 x 10 = -30, taken in the year.
    EXPECT_EQ(lists[0].mSaleGains, (std::vector<std::string>{"-30"}));
}

// A restart with a file of the other broker can remove a question that the files now answer.
TEST(MixedBrokerIntegrationTest, APurchaseInAnAddedIbkrFileRemovesTheLossQuestion) {
    constexpr std::string_view buy =
        R"(2025-01-15T09:00:00Z,2025-01-15,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sell =
        R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)";

    std::optional<ApplicationRun> run =
        startRun(requestOf(2025, {tradeRepublicFile("tr.csv", {buy, sell})}));

    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(run->result().mRequests[0].mType, RequestType::LossEligibilityConfirmationRequired);

    const std::string requestId = run->result().mRequests[0].mId;

    EXPECT_TRUE(answerRequest(*run, requestId, NoOtherActivityAnswer{}).empty());

    // 10 units bought at Interactive Brokers on 2025-02-20 are within 30 days before the sale.
    EXPECT_FALSE(
        run->restart(requestOf(2025,
                               {tradeRepublicFile("tr.csv", {buy, sell}),
                                ibkrFile("ibkr.xml",
                                         R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades>
        <Trade transactionID="ib-buy-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20250220" dateTime="20250220;040000" buySell="BUY" quantity="10" tradePrice="25" proceeds="-250" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
      </Trades>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)xml")}))
            .has_value());

    const ApplicationResult& result = run->result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);
    ASSERT_TRUE(result.report(ReportType::CapitalGains).mXml.has_value());
    expectValidForKdvpSchemas(*result.report(ReportType::CapitalGains).mXml);
    EXPECT_TRUE(contains(*result.report(ReportType::CapitalGains).mXml, "<F10>false</F10>"));

    // The answer of the first run is kept in the list, but no stage asked its question again.
    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_TRUE(result.mDecisions[0].mFromEarlierRun);
    EXPECT_FALSE(result.mDecisions[0].mUsed);
}

// calculations.md: every income amount is converted at the official rate of its date, whatever the
// broker; the fee summary keeps each broker's own fee currency.
TEST(MixedBrokerIntegrationTest, OneDateAndCurrencyUseTheSameOfficialRateAtBothBrokers) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2024,
        {tradeRepublicFile(
             "tr.csv",
             {
                 // USD 10.00 at the broker rate 0.90 would be EUR 9.00; the official rate is used.
                 R"(2024-05-16T10:00:00Z,2024-05-16,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,9.00,,,EUR,10.00,USD,0.900000,,synthetic-dividend-1,,,,)",
                 R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000003,1,10,-10,-1.00,,EUR,,,,,synthetic-buy-1,,,,)",
             }),
         ibkrFile("ibkr.xml", R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades>
        <Trade transactionID="ib-buy-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000004" currency="USD" tradeDate="20240201" dateTime="20240201;040000" buySell="BUY" quantity="1" tradePrice="10" proceeds="-10" ibCommission="-2" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>
      </Trades>
      <CashTransactions>
        <CashTransaction transactionID="ib-dividend-1" actionID="A1" levelOfDetail="DETAIL" type="Dividends" assetCategory="STK" symbol="SYNB" description="SYNB CASH DIVIDEND USD 10.00" isin="XX0000000002" currency="USD" dateTime="20240516" settleDate="20240516" reportDate="20240516" amount="10" issuerCountryCode=""/>
      </CashTransactions>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)xml")}));

    ASSERT_TRUE(run.has_value());

    const ApplicationResult& result = run->result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Generated);
    ASSERT_TRUE(result.report(ReportType::Dividends).mIncome.has_value());
    ASSERT_EQ(result.report(ReportType::Dividends).mIncome->mRows.size(), 2U);

    // The ECB rate of 2024-05-16 is 1.0866 USD per EUR: USD 10 / 1.0866 = EUR 9.2030 (9.20302...).
    EXPECT_EQ(result.report(ReportType::Dividends).mIncome->mRows[0].mIsin, "XX0000000001");
    EXPECT_EQ(result.report(ReportType::Dividends).mIncome->mRows[0].mGross, 92'030);
    EXPECT_EQ(result.report(ReportType::Dividends).mIncome->mRows[1].mIsin, "XX0000000002");
    EXPECT_EQ(result.report(ReportType::Dividends).mIncome->mRows[1].mGross, 92'030);

    // One warning for each dividend: the payer of an XX ISIN is not in the reference files. Nothing
    // else is reported, in particular nothing about the broker rate 0.90 of Trade Republic.
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics[0].mMessageKey, MessageKey::DividendPayerNotFound);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics[0].mIsin, "XX0000000001");
    EXPECT_EQ(result.mDiagnostics[1].mMessageKey, MessageKey::DividendPayerNotFound);
    EXPECT_EQ(result.mDiagnostics[1].mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics[1].mIsin, "XX0000000002");

    ASSERT_TRUE(result.report(ReportType::Dividends).mXml.has_value());
    expectValidForDivSchema(*result.report(ReportType::Dividends).mXml);

    // Fees: Trade Republic EUR 1.00; Interactive Brokers USD 2.00 with no broker rate, so its EUR
    // total and the overall EUR total are not available.
    ASSERT_EQ(result.mFees.mBrokers.size(), 2U);
    EXPECT_EQ(result.mFees.mBrokers[0].mBroker, Broker::TradeRepublic);
    ASSERT_EQ(result.mFees.mBrokers[0].mByCurrency.size(), 1U);
    EXPECT_EQ(result.mFees.mBrokers[0].mByCurrency[0].mCurrency, Currency::EUR);
    EXPECT_EQ(result.mFees.mBrokers[0].mByCurrency[0].mTotal, 10'000); // EUR 1.00
    EXPECT_EQ(result.mFees.mBrokers[0].mEurTotal, 10'000);

    EXPECT_EQ(result.mFees.mBrokers[1].mBroker, Broker::InteractiveBrokers);
    ASSERT_EQ(result.mFees.mBrokers[1].mByCurrency.size(), 1U);
    EXPECT_EQ(result.mFees.mBrokers[1].mByCurrency[0].mCurrency, Currency::USD);
    EXPECT_EQ(result.mFees.mBrokers[1].mByCurrency[0].mTotal, 20'000); // USD 2.00
    EXPECT_FALSE(result.mFees.mBrokers[1].mEurTotal.has_value());
    EXPECT_FALSE(result.mFees.mEurTotal.has_value());
}

// Mirror of the exclusion scenario of the Trade Republic pipeline test, with the failures at
// Interactive Brokers: an exclusion removes only what failed, and no income or loss of the other
// broker.
TEST(MixedBrokerIntegrationTest, ExclusionsAtOneBrokerKeepTheFormsAndLossesOfTheOther) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2024,
        {tradeRepublicFile(
             "tr.csv",
             {
                 R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
                 R"(2024-03-04T09:00:00Z,2024-03-04,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-5,8,40,,,EUR,,,,,synthetic-sell-1,,,,)",
                 R"(2024-01-31T10:00:00Z,2024-01-31,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,0.33,,,EUR,,,,Synthetic bank interest,synthetic-interest-1,,,,)",
                 R"(2024-02-29T10:00:00Z,2024-02-29,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,0.33,,,EUR,,,,Synthetic bank interest,synthetic-interest-2,,,,)",
                 R"(2024-03-28T10:00:00Z,2024-03-28,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,0.34,,,EUR,,,,Synthetic bank interest,synthetic-interest-3,,,,)",
                 R"(2024-03-10T10:00:00Z,2024-03-10,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond B,XS0000000002,,,10.00,,,EUR,,,,Interest payment,synthetic-coupon-1,,,,)",
             }),
         ibkrFile("ibkr.xml", R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades>
        <Trade transactionID="ib-buy-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20240320" dateTime="20240320;040000" buySell="BUY" quantity="5" tradePrice="8" proceeds="-40" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
        <Trade transactionID="ib-sell-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Other Share" isin="XX0000000002" currency="EUR" tradeDate="20240402" dateTime="20240402;040000" buySell="SELL" quantity="-5" tradePrice="10" proceeds="50" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
      </Trades>
      <CashTransactions>
        <CashTransaction transactionID="ib-interest-1" actionID="" levelOfDetail="DETAIL" type="Broker Interest Received" assetCategory="CASH" symbol="" description="EUR CREDIT INT FOR MAR-2024" isin="" currency="EUR" dateTime="20240415" settleDate="20240415" reportDate="20240415" amount="2.5" issuerCountryCode=""/>
        <CashTransaction transactionID="ib-coupon-1" actionID="" levelOfDetail="DETAIL" type="Bond Interest Received" assetCategory="BOND" symbol="SYNBOND" description="SYNBOND 5% 2030 COUPON" isin="XX0000000003" currency="RUB" dateTime="20240801" settleDate="20240801" reportDate="20240801" amount="200" issuerCountryCode=""/>
        <CashTransaction transactionID="ib-dividend-1" actionID="A1" levelOfDetail="DETAIL" type="Dividends" assetCategory="STK" symbol="SYNC" description="SYNC CASH DIVIDEND EUR 10.00" isin="XX0000000004" currency="EUR" dateTime="20240502" settleDate="20240502" reportDate="20240502" amount="10" issuerCountryCode=""/>
      </CashTransactions>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)xml")}));

    ASSERT_TRUE(run.has_value());

    // Three questions, ordered by source, date and report (diagnostics.md): the deposit total
    // belongs to the Trade Republic file (source 0); the capital failure (the sale of
    // XX0000000002 has no purchase) and the Doh-Obr failure (no ECB rate for RUB in 2024) belong
    // to the Interactive Brokers file (source 1), the sale of 2024-04-02 before the coupon of
    // 2024-08-01.
    const ApplicationResult& before = run->result();

    ASSERT_EQ(before.mRequests.size(), 3U);
    EXPECT_EQ(before.mRequests[0].mType, RequestType::DepositInterestTotalRequired);
    EXPECT_EQ(before.mRequests[1].mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_EQ(before.mRequests[2].mType, RequestType::OtherInterestFailureResolutionRequired);
    EXPECT_EQ(before.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(before.report(ReportType::OtherInterest).mStatus, ReportStatus::NeedsInput);

    // Doh-DHO is made as for yes while its question is open.
    EXPECT_EQ(before.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Generated);
    ASSERT_TRUE(before.report(ReportType::BankDepositInterest).mXml.has_value());

    const std::string depositBefore = *before.report(ReportType::BankDepositInterest).mXml;

    std::vector<DecisionAnswer> answers;

    answers.push_back({before.mRequests[1].mId,
                       before.mRunRevision,
                       FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments}});
    answers.push_back({before.mRequests[2].mId,
                       before.mRunRevision,
                       FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments}});

    EXPECT_TRUE(run->session().answer(std::move(answers)).empty());

    const ApplicationResult& result = run->result();

    // Doh-KDVP lists XX0000000001 and leaves out XX0000000002 only. The 5 units sold at a loss on
    // 2024-03-04 are replaced by the 5 units bought at Interactive Brokers on 2024-03-20.
    const ReportResult& capital = result.report(ReportType::CapitalGains);

    EXPECT_EQ(capital.mStatus, ReportStatus::Generated);
    ASSERT_EQ(capital.mExcludedInvestments.size(), 1U);
    EXPECT_EQ(capital.mExcludedInvestments[0].mIsin, "XX0000000002");
    ASSERT_TRUE(capital.mXml.has_value());
    expectValidForKdvpSchemas(*capital.mXml);
    EXPECT_TRUE(contains(*capital.mXml, "<F10>false</F10>"));
    EXPECT_FALSE(contains(*capital.mXml, "<F10>true</F10>"));

    // Doh-Obr: the coupon of Trade Republic (EUR 10.00) and the cash interest of Interactive
    // Brokers (EUR 2.50); only XX0000000003 is left out.
    const ReportResult& other = result.report(ReportType::OtherInterest);

    EXPECT_EQ(other.mStatus, ReportStatus::Generated);
    ASSERT_EQ(other.mExcludedInvestments.size(), 1U);
    EXPECT_EQ(other.mExcludedInvestments[0].mIsin, "XX0000000003");
    EXPECT_TRUE(other.mExcludedBrokers.empty());
    ASSERT_TRUE(other.mXml.has_value());
    expectValidForObrSchema(*other.mXml);
    ASSERT_TRUE(other.mIncome.has_value());
    ASSERT_EQ(other.mIncome->mRows.size(), 2U);

    EXPECT_EQ(other.mIncome->mRows[0].mKind, IncomeRowKind::BondCoupon);
    EXPECT_EQ(other.mIncome->mRows[0].mIsin, "XS0000000002");
    EXPECT_EQ(other.mIncome->mRows[0].mGross, 100'000); // EUR 10.00

    EXPECT_EQ(other.mIncome->mRows[1].mKind, IncomeRowKind::BrokerCashInterest);
    EXPECT_EQ(other.mIncome->mRows[1].mBroker, Broker::InteractiveBrokers);
    EXPECT_EQ(other.mIncome->mRows[1].mGross, 25'000); // EUR 2.50
    EXPECT_EQ(other.mIncome->mRows[1].mPayerName, "Interactive Brokers Ireland Limited");

    EXPECT_EQ(other.mIncome->mGross, 125'000); // EUR 10.00 + EUR 2.50

    // Doh-DHO is the same document as before: the deposit interest of EUR 1.00 only.
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Generated);
    ASSERT_TRUE(result.report(ReportType::BankDepositInterest).mXml.has_value());
    EXPECT_EQ(*result.report(ReportType::BankDepositInterest).mXml, depositBefore);
    expectValidForDhoSchema(depositBefore);

    // Doh-Div has the dividend of EUR 10.00 of Interactive Brokers.
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Generated);
    ASSERT_TRUE(result.report(ReportType::Dividends).mIncome.has_value());
    ASSERT_EQ(result.report(ReportType::Dividends).mIncome->mRows.size(), 1U);
    EXPECT_EQ(result.report(ReportType::Dividends).mIncome->mRows[0].mGross, 100'000); // EUR 10.00

    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);
}

} // namespace
