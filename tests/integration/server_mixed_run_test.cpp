// Mixed-broker runs and the answers no other HTTP test sends, through a real server. The Trade
// Republic and Interactive Brokers files are the synthetic fixtures of
// `tests/test_data/pipeline_mixed`; the expected values are those of its README and its golden
// file. The tests of the answers that no other HTTP test sends use small inline files.

#include "../support/kdvp_row_fifo.hpp"
#include "../support/xsd_validation.hpp"
#include "server_run_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;
using namespace taxbroker::servertest;
using taxbroker::test::expectValidForKdvpSchemas;
using taxbroker::test::fifoOverKdvpRows;
using taxbroker::test::KdvpListFifo;

/// One broker file of an upload: the broker name of the request, the file name and the bytes.
struct BrokerUploadFile {
    std::string mBroker;
    std::string mFileName;
    std::string mContent;
};

/// An upload of the given broker files, in this order (`file0`, `file1`, ...).
PipelineUpload mixedUpload(int aYear, const std::vector<BrokerUploadFile>& aFiles) {
    std::string list = "[";
    std::vector<UploadFile> files;

    files.reserve(aFiles.size());

    for (std::size_t index = 0; index < aFiles.size(); ++index)
    {
        const std::string part = "file" + std::to_string(index);

        if (index > 0)
        {
            list += ",";
        }

        list += R"({"part":")" + part + R"(","kind":"broker","broker":")" + aFiles[index].mBroker +
                R"("})";
        files.push_back({part, aFiles[index].mFileName, aFiles[index].mContent});
    }

    list += "]";

    return {multipartBody(requestJson(aYear, list), files), contentTypeHeader()};
}

/// The committed Trade Republic file of 2021 to 2024.
BrokerUploadFile tradeRepublicFixture() {
    return {"trade_republic",
            "trade_republic_2021_2024.csv",
            readTestData("pipeline_mixed", "trade_republic_2021_2024.csv")};
}

/// The committed Interactive Brokers file of 2023 and 2024.
BrokerUploadFile ibkr20232024Fixture() {
    return {"interactive_brokers",
            "ibkr_2023_2024.xml",
            readTestData("pipeline_mixed", "ibkr_2023_2024.xml")};
}

/// The committed Interactive Brokers file of 2025: the replacement purchase.
BrokerUploadFile ibkr2025Fixture() {
    return {"interactive_brokers",
            "ibkr_2025.xml",
            readTestData("pipeline_mixed", "ibkr_2025.xml")};
}

/// An Interactive Brokers statement with the given trade, corporate action and cash rows.
std::string
ibkrStatement(std::string_view aTrades, std::string_view aActions, std::string_view aCash) {
    std::string xml = "<FlexQueryResponse queryName=\"synthetic\" type=\"AF\">\n"
                      "  <FlexStatements count=\"1\">\n"
                      "    <FlexStatement accountId=\"U0000000\">\n"
                      "      <AccountInformation accountId=\"U0000000\" ibEntity=\"IB-IE\"/>\n"
                      "      <Trades>\n";

    xml += aTrades;
    xml += "\n      </Trades>\n      <CorporateActions>\n";
    xml += aActions;
    xml += "\n      </CorporateActions>\n      <CashTransactions>\n";
    xml += aCash;
    xml += "\n      </CashTransactions>\n    </FlexStatement>\n  </FlexStatements>\n"
           "</FlexQueryResponse>\n";

    return xml;
}

// The answers of the mixed scenario, one call each: the revision of the run they answer is in the
// body. The identity answer says that rows 1 and 2 are one action and row 3 is another.
constexpr std::string_view kGroupingAndLossAnswers =
    R"({"runRevision":1,"answers":[)"
    R"({"requestId":"request-0001","answer":{"type":"grouping","actionOfRow":[1,1,2]}},)"
    R"({"requestId":"request-0002","answer":"no_other_activity"}]})";
constexpr std::string_view kFirstRatioAnswer =
    R"({"runRevision":2,"answers":[{"requestId":"request-0001","answer":)"
    R"({"newShares":2,"oldShares":1,"effectiveDate":"2024-11-10","confirmedUnits":"20"}}]})";
constexpr std::string_view kSecondRatioAnswer =
    R"({"runRevision":3,"answers":[{"requestId":"request-0001","answer":)"
    R"({"newShares":2,"oldShares":1,"effectiveDate":"2024-11-20","confirmedUnits":"40"}}]})";

/// The broker of each source ID named by a request of the result.
std::map<std::string, std::string> brokerOfEachSourceInRequests(const Json& aResult) {
    std::map<std::string, std::string> brokers;

    for (const Json& request : aResult["requests"])
    {
        for (const Json& source : request["sources"])
        {
            brokers[source["sourceId"]] = source["broker"];
        }
    }

    return brokers;
}

/// The sanitized file name of each source ID named by a request of the result.
std::map<std::string, std::string> fileOfEachSourceInRequests(const Json& aResult) {
    std::map<std::string, std::string> files;

    for (const Json& request : aResult["requests"])
    {
        for (const Json& source : request["sources"])
        {
            files[source["sourceId"]] = source["file"];
        }
    }

    return files;
}

/// How many times the text is in the XML.
std::size_t countOf(const std::string& aXml, std::string_view aText) {
    std::size_t count = 0;
    std::size_t position = aXml.find(aText);

    while (position != std::string::npos)
    {
        ++count;
        position = aXml.find(aText, position + aText.size());
    }

    return count;
}

// ---- Mixed-broker files

TEST_F(ServerRunTest, AMixedUploadAsksTheIdentityAndTheLossQuestion) {
    const auto server = startServer();

    const Response uploaded =
        server->upload(mixedUpload(2024, {tradeRepublicFixture(), ibkr20232024Fixture()}));

    ASSERT_EQ(uploaded.result_int(), 200U);

    const Json result = jsonOf(uploaded);

    EXPECT_EQ(result["runRevision"], 1);
    EXPECT_EQ(result["status"], "needs_input");
    EXPECT_EQ(result["reports"]["capitalGains"]["status"], "needs_input");
    EXPECT_EQ(result["reports"]["dividends"]["status"], "no_data");
    EXPECT_EQ(result["reports"]["bankInterest"]["status"], "no_data");
    EXPECT_EQ(result["reports"]["otherInterest"]["status"], "no_data");

    // The sources are the two files in upload order; the Interactive Brokers rows are all in the
    // second file.
    EXPECT_EQ(brokerOfEachSourceInRequests(result),
              (std::map<std::string, std::string>{{"source-0001", "trade_republic"},
                                                  {"source-0002", "interactive_brokers"}}));
    EXPECT_EQ(fileOfEachSourceInRequests(result),
              (std::map<std::string, std::string>{{"source-0001", "trade_republic_2021_2024.csv"},
                                                  {"source-0002", "ibkr_2023_2024.xml"}}));

    ASSERT_EQ(result["requests"].size(), 2U);

    // XX0000000202: the Trade Republic split of 2024-11-10 and the two Interactive Brokers splits
    // of 2024-11-14 and 2024-11-20 are within 10 days, so the files cannot say which rows are one
    // action. The rows are in date order.
    const Json& identity = result["requests"][0];

    EXPECT_EQ(identity["id"], "request-0001");
    EXPECT_EQ(identity["type"], "corporate_action_identity_required");
    EXPECT_EQ(identity["isin"], "XX0000000202");

    ASSERT_EQ(identity["identityCheck"]["rows"].size(), 3U);
    EXPECT_EQ(identity["identityCheck"]["rows"][0]["broker"], "trade_republic");
    EXPECT_EQ(identity["identityCheck"]["rows"][0]["date"], "2024-11-10");
    EXPECT_EQ(identity["identityCheck"]["rows"][0]["transactionId"], "m-split-1");
    EXPECT_EQ(identity["identityCheck"]["rows"][1]["broker"], "interactive_brokers");
    EXPECT_EQ(identity["identityCheck"]["rows"][1]["date"], "2024-11-14");
    EXPECT_EQ(identity["identityCheck"]["rows"][1]["brokerActionId"], "S1");
    EXPECT_EQ(identity["identityCheck"]["rows"][2]["broker"], "interactive_brokers");
    EXPECT_EQ(identity["identityCheck"]["rows"][2]["date"], "2024-11-20");
    EXPECT_EQ(identity["identityCheck"]["rows"][2]["brokerActionId"], "S2");

    // XX0000000203: the loss of the sale of 4 units on 2024-12-20 has no replacement known, so the
    // 4 units are open.
    const Json& loss = result["requests"][1];

    EXPECT_EQ(loss["id"], "request-0002");
    EXPECT_EQ(loss["type"], "loss_eligibility_confirmation_required");
    EXPECT_EQ(loss["isin"], "XX0000000203");
    ASSERT_EQ(loss["lossCheck"]["sales"].size(), 1U);
    EXPECT_EQ(loss["lossCheck"]["sales"][0]["saleDate"], "2024-12-20");
    EXPECT_EQ(loss["lossCheck"]["sales"][0]["openUnits"], "4.00000000");

    // The form has no file while it needs input.
    EXPECT_EQ(server->send("GET", "/api/run/reports/capitalGains/xml?revision=1").result_int(),
              404U);
}

TEST_F(ServerRunTest, AnsweringTheMixedQuestionsOverHttpGivesTheGoldenDohKdvp) {
    const auto server = startServer();

    ASSERT_EQ(server->upload(mixedUpload(2024, {tradeRepublicFixture(), ibkr20232024Fixture()}))
                  .result_int(),
              200U);

    // Rows 1 and 2 (Trade Republic 2024-11-10 and Interactive Brokers S1) are one split, row 3
    // (S2) is another; the loss question is answered in the same call.
    const Response grouped =
        server->postJson("/api/run/answers", std::string{kGroupingAndLossAnswers});

    ASSERT_EQ(grouped.result_int(), 200U);

    const Json afterGrouping = jsonOf(grouped);

    EXPECT_EQ(afterGrouping["runRevision"], 2);
    EXPECT_EQ(afterGrouping["status"], "needs_input");

    // The first action has the rows of both brokers and 10 units before 2024-11-10; no question
    // asks whether the rows are one split.
    ASSERT_EQ(afterGrouping["requests"].size(), 1U);

    const Json& firstRatio = afterGrouping["requests"][0];

    EXPECT_EQ(firstRatio["type"], "corporate_action_ratio_required");
    EXPECT_EQ(firstRatio["ratioCheck"]["effectiveDate"], "2024-11-10");
    EXPECT_EQ(firstRatio["ratioCheck"]["positionBefore"], "10.00000000");
    EXPECT_EQ(firstRatio["ratioCheck"]["askSameEvent"], false);
    ASSERT_EQ(firstRatio["ratioCheck"]["sources"].size(), 2U);
    EXPECT_EQ(firstRatio["ratioCheck"]["sources"][0]["broker"], "trade_republic");
    EXPECT_EQ(firstRatio["ratioCheck"]["sources"][1]["broker"], "interactive_brokers");

    // 10 units with 2 : 1 are 20 units; the second action is asked with them and has only the
    // Interactive Brokers row of 2024-11-20.
    const Response first = server->postJson("/api/run/answers", std::string{kFirstRatioAnswer});

    ASSERT_EQ(first.result_int(), 200U);

    const Json afterFirst = jsonOf(first);

    EXPECT_EQ(afterFirst["runRevision"], 3);
    ASSERT_EQ(afterFirst["requests"].size(), 1U);
    EXPECT_EQ(afterFirst["requests"][0]["ratioCheck"]["effectiveDate"], "2024-11-20");
    EXPECT_EQ(afterFirst["requests"][0]["ratioCheck"]["positionBefore"], "20.00000000");
    ASSERT_EQ(afterFirst["requests"][0]["ratioCheck"]["sources"].size(), 1U);
    EXPECT_EQ(afterFirst["requests"][0]["ratioCheck"]["sources"][0]["broker"],
              "interactive_brokers");

    const Response second = server->postJson("/api/run/answers", std::string{kSecondRatioAnswer});

    ASSERT_EQ(second.result_int(), 200U);

    const Json done = jsonOf(second);

    // Both actions are applied once (10 x 2 x 2 = 40 units). The warning group of the rows of one
    // year from two files stays.
    EXPECT_EQ(done["runRevision"], 4);
    EXPECT_TRUE(done["requests"].empty());
    EXPECT_EQ(done["status"], "completed_with_warnings");
    EXPECT_EQ(done["reports"]["capitalGains"]["status"], "generated");

    std::vector<std::string> codes;

    std::transform(done["diagnostics"].begin(),
                   done["diagnostics"].end(),
                   std::back_inserter(codes),
                   [](const Json& aDiagnostic) { return aDiagnostic["code"].get<std::string>(); });

    EXPECT_EQ(codes,
              (std::vector<std::string>{"corporate_action_applied",
                                        "corporate_action_sources_mismatch_possible",
                                        "corporate_action_applied"}));

    const Response download = server->send("GET", "/api/run/reports/capitalGains/xml?revision=4");

    ASSERT_EQ(download.result_int(), 200U);
    EXPECT_EQ(download.body(), readTestData("pipeline_mixed", "Doh-KDVP_2024.golden.xml"));
    expectValidForKdvpSchemas(download.body());

    // The FIFO over the written rows: one list for each ISIN, in the order of the ISINs.
    const std::vector<KdvpListFifo> lists = fifoOverKdvpRows(download.body());

    ASSERT_EQ(lists.size(), 3U);

    // XX0000000201: F8 is 1.1, then 1.1 + 0.5 = 1.6, then 1.6 - 1.6 = 0. The gain is
    // 1.6 x 14 - (1.1 x 10 + 0.5 x 11) = 22.40 - 16.50 = EUR 5.90.
    EXPECT_EQ(lists[0].mIsin, "XX0000000201");
    EXPECT_TRUE(lists[0].mProblems.empty());
    EXPECT_EQ(lists[0].mWrittenF8, (std::vector<std::string>{"1.1", "1.6", "0"}));
    EXPECT_EQ(lists[0].mSaleGains, (std::vector<std::string>{"5.9"}));

    // XX0000000202: 10 units at EUR 10 are 40 units at EUR 2.50 after the two 2 : 1 splits. The
    // sale of 1 unit at EUR 6 is a gain of 6 - 2.50 = EUR 3.50.
    EXPECT_EQ(lists[1].mIsin, "XX0000000202");
    EXPECT_TRUE(lists[1].mProblems.empty());
    EXPECT_EQ(lists[1].mWrittenF8, (std::vector<std::string>{"1", "0"}));
    EXPECT_EQ(lists[1].mSaleGains, (std::vector<std::string>{"3.5"}));

    // XX0000000203: 4 units bought at EUR 10 and sold at EUR 8 are a loss of 4 x 8 - 4 x 10 =
    // EUR -8, taken in full because the loss answer says there is no other activity.
    EXPECT_EQ(lists[2].mIsin, "XX0000000203");
    EXPECT_TRUE(lists[2].mProblems.empty());
    EXPECT_EQ(lists[2].mWrittenF8, (std::vector<std::string>{"4", "0"}));
    EXPECT_EQ(lists[2].mSaleGains, (std::vector<std::string>{"-8"}));

    // The 2023 sale of XX0000000201 and the splits have no row.
    EXPECT_EQ(countOf(download.body(), "<Sale>"), 3U);
    EXPECT_EQ(countOf(download.body(), "<F10>true</F10>"), 3U);
    EXPECT_EQ(countOf(download.body(), "<F6>2023-04-05</F6>"), 0U);
}

TEST_F(ServerRunTest, EitherBrokerOrderGivesTheSameMixedDohKdvp) {
    const auto server = startServer();

    // The Interactive Brokers file first: it is source-0001 now.
    const Json uploaded =
        jsonOf(server->upload(mixedUpload(2024, {ibkr20232024Fixture(), tradeRepublicFixture()})));

    EXPECT_EQ(brokerOfEachSourceInRequests(uploaded),
              (std::map<std::string, std::string>{{"source-0001", "interactive_brokers"},
                                                  {"source-0002", "trade_republic"}}));

    // The requests and the answers are the same as with the other order.
    ASSERT_EQ(uploaded["requests"].size(), 2U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "corporate_action_identity_required");
    EXPECT_EQ(uploaded["requests"][1]["type"], "loss_eligibility_confirmation_required");

    ASSERT_EQ(
        server->postJson("/api/run/answers", std::string{kGroupingAndLossAnswers}).result_int(),
        200U);
    ASSERT_EQ(server->postJson("/api/run/answers", std::string{kFirstRatioAnswer}).result_int(),
              200U);

    const Json done = jsonOf(server->postJson("/api/run/answers", std::string{kSecondRatioAnswer}));

    EXPECT_EQ(done["runRevision"], 4);
    EXPECT_TRUE(done["requests"].empty());

    const Response download = server->send("GET", "/api/run/reports/capitalGains/xml?revision=4");

    ASSERT_EQ(download.result_int(), 200U);
    EXPECT_EQ(download.body(), readTestData("pipeline_mixed", "Doh-KDVP_2024.golden.xml"));
}

TEST_F(ServerRunTest, AnAddedIbkrFileInANewUploadRemovesTheLossQuestion) {
    const auto server = startServer();

    ASSERT_EQ(server->upload(mixedUpload(2024, {tradeRepublicFixture(), ibkr20232024Fixture()}))
                  .result_int(),
              200U);
    ASSERT_EQ(
        server->postJson("/api/run/answers", std::string{kGroupingAndLossAnswers}).result_int(),
        200U);
    ASSERT_EQ(server->postJson("/api/run/answers", std::string{kFirstRatioAnswer}).result_int(),
              200U);
    ASSERT_EQ(server->postJson("/api/run/answers", std::string{kSecondRatioAnswer}).result_int(),
              200U);

    // The new upload adds the Interactive Brokers file of 2025: 4 units of XX0000000203 bought on
    // 2025-01-06, 17 days after the loss sale of 2024-12-20, are replacement units for all 4 units.
    const Json restarted = jsonOf(server->upload(
        mixedUpload(2024, {tradeRepublicFixture(), ibkr20232024Fixture(), ibkr2025Fixture()})));

    EXPECT_EQ(restarted["runRevision"], 5);
    EXPECT_TRUE(restarted["requests"].empty());
    EXPECT_EQ(restarted["reports"]["capitalGains"]["status"], "generated");

    // The loss answer is kept from the earlier run but not used any more. The identity answer and
    // both ratio answers are used, so nothing is asked again.
    ASSERT_EQ(restarted["decisions"].size(), 4U);
    EXPECT_EQ(restarted["decisions"][0]["type"], "corporate_action_identity_required");
    EXPECT_EQ(restarted["decisions"][0]["fromEarlierRun"], true);
    EXPECT_EQ(restarted["decisions"][0]["used"], true);
    EXPECT_EQ(restarted["decisions"][1]["type"], "loss_eligibility_confirmation_required");
    EXPECT_EQ(restarted["decisions"][1]["fromEarlierRun"], true);
    EXPECT_EQ(restarted["decisions"][1]["used"], false);
    EXPECT_EQ(restarted["decisions"][2]["type"], "corporate_action_ratio_required");
    EXPECT_EQ(restarted["decisions"][2]["fromEarlierRun"], true);
    EXPECT_EQ(restarted["decisions"][2]["used"], true);
    EXPECT_EQ(restarted["decisions"][3]["type"], "corporate_action_ratio_required");
    EXPECT_EQ(restarted["decisions"][3]["fromEarlierRun"], true);
    EXPECT_EQ(restarted["decisions"][3]["used"], true);

    const Response download = server->send("GET", "/api/run/reports/capitalGains/xml?revision=5");

    ASSERT_EQ(download.result_int(), 200U);

    // The sale of XX0000000203 is now `F10` false (the loss is not recognized); the other two sales
    // stay `F10` true. The purchase of 2025 is no row of the 2024 form.
    EXPECT_EQ(countOf(download.body(), "<F10>true</F10>"), 2U);
    EXPECT_EQ(countOf(download.body(), "<F10>false</F10>"), 1U);
    EXPECT_EQ(countOf(download.body(), "<F1>2025-01-06</F1>"), 0U);
    expectValidForKdvpSchemas(download.body());

    // An answer for revision 4, the one before the new upload, is stale and changes nothing.
    const Response stale = server->postJson(
        "/api/run/answers",
        R"({"runRevision":4,"answers":[{"requestId":"request-0002","answer":"no_other_activity"}]})");

    ASSERT_EQ(stale.result_int(), 422U);
    EXPECT_EQ(jsonOf(stale)["rejections"][0]["messageKey"], "answer_rejected.stale_revision");
    EXPECT_EQ(jsonOf(server->send("GET", "/api/run"))["runRevision"], 5);
}

TEST_F(ServerRunTest, AGroupingWithAGapIsRejectedAndChangesNothing) {
    const auto server = startServer();

    ASSERT_EQ(server->upload(mixedUpload(2024, {tradeRepublicFixture(), ibkr20232024Fixture()}))
                  .result_int(),
              200U);

    // The numbers of the actions must run from 1 without gaps: [1, 1, 3] has no action 2.
    const Response rejected =
        server->postJson("/api/run/answers",
                         R"({"runRevision":1,"answers":[{"requestId":"request-0001","answer":)"
                         R"({"type":"grouping","actionOfRow":[1,1,3]}}]})");

    ASSERT_EQ(rejected.result_int(), 422U);
    EXPECT_EQ(jsonOf(rejected)["rejections"][0]["messageKey"],
              "answer_rejected.grouping_incomplete");

    const Json current = jsonOf(server->send("GET", "/api/run"));

    EXPECT_EQ(current["runRevision"], 1);
    ASSERT_EQ(current["requests"].size(), 2U);
    EXPECT_EQ(current["requests"][0]["type"], "corporate_action_identity_required");
    EXPECT_EQ(current["requests"][1]["type"], "loss_eligibility_confirmation_required");
}

// ---- Answers that no other HTTP test sends

TEST_F(ServerRunTest, SeparateEventsIsAnsweredOverHttpAndAsksTheFirstSplitAlone) {
    // Trade Republic books a split on 2024-11-10 and Interactive Brokers one on 2024-11-14: 4 days
    // apart, so they could be one split that each broker booked on its own date.
    constexpr std::string_view buy =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view split =
        R"(2024-11-10T09:00:00Z,2024-11-10,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,10,,,,,,,,,,synthetic-split-1,,,,)";
    constexpr std::string_view sale =
        R"(2024-12-02T09:00:00Z,2024-12-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,6,6,,,EUR,,,,,synthetic-sell-1,,,,)";
    const std::string ibkrXml = ibkrStatement(
        "",
        R"(<CorporateAction accountId="U0000000" transactionID="synthetic-ibkr-split-1" actionID="S1" type="FS" assetCategory="STK" description="Synthetic Share" isin="XX0000000001" currency="EUR" dateTime="20241114;202500" reportDate="20241114" quantity="10" proceeds="0" value="0"/>)",
        "");

    const auto server = startServer();

    const Json uploaded = jsonOf(server->upload(
        mixedUpload(2024,
                    {{"trade_republic", "history.csv", csvOfRows({buy, split, sale})},
                     {"interactive_brokers", "ibkr.xml", ibkrXml}})));

    // One question: are the two rows one split? It lists the rows of both brokers.
    ASSERT_EQ(uploaded["requests"].size(), 1U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "corporate_action_ratio_required");
    EXPECT_EQ(uploaded["requests"][0]["ratioCheck"]["askSameEvent"], true);
    EXPECT_EQ(uploaded["requests"][0]["ratioCheck"]["sources"].size(), 2U);

    const Response answered =
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("separate_events")"));

    ASSERT_EQ(answered.result_int(), 200U);

    const Json result = jsonOf(answered);

    // Two splits: the first is asked alone, with the 10 units before 2024-11-10 and the dates up
    // to the day before the second row.
    EXPECT_EQ(result["runRevision"], 2);
    ASSERT_EQ(result["requests"].size(), 1U);
    EXPECT_EQ(result["requests"][0]["type"], "corporate_action_ratio_required");
    EXPECT_EQ(result["requests"][0]["ratioCheck"]["askSameEvent"], false);
    EXPECT_EQ(result["requests"][0]["ratioCheck"]["effectiveDate"], "2024-11-10");
    EXPECT_EQ(result["requests"][0]["ratioCheck"]["positionBefore"], "10.00000000");
    ASSERT_EQ(result["requests"][0]["ratioCheck"]["sources"].size(), 1U);
    EXPECT_EQ(result["requests"][0]["ratioCheck"]["sources"][0]["broker"], "trade_republic");
    EXPECT_EQ(result["requests"][0]["ratioCheck"]["dateRanges"].back()["latestDate"], "2024-11-13");
}

TEST_F(ServerRunTest, ADeferredStockMergerIsAnsweredOverHttpAndCarriesTheLots) {
    // 10 units of A bought at EUR 40 on 2023-03-01 become 4 units of B on 2024-06-03; the answer
    // says the exchange is deferred. The lot carries on under B: 4 units at EUR 400 / 4 = EUR 100,
    // purchase date 2023-03-01, method `E`. B is sold on 2025-02-03 at EUR 140.
    const std::string ibkrXml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYN" description="SYNTHETIC MERGED SHARE" isin="XX0000000136" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="10" tradePrice="40" proceeds="-400" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1002" tradeID="1002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYN" description="SYNTHETIC ACQUIRER" isin="XX0000000137" currency="EUR" tradeDate="20250203" dateTime="20250203;100000" buySell="SELL" quantity="-4" tradePrice="140" proceeds="560" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="3001" actionID="M7001" type="TC" assetCategory="STK" symbol="SYN" description="SYNTHETIC MERGED SHARE" isin="XX0000000136" currency="EUR" dateTime="20240603;202500" reportDate="20240603" quantity="-10" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="3002" actionID="M7001" type="TC" assetCategory="STK" symbol="SYN" description="SYNTHETIC ACQUIRER" isin="XX0000000137" currency="EUR" dateTime="20240603;202500" reportDate="20240603" quantity="4" proceeds="0" value="0"/>)",
        "");

    const auto server = startServer();

    const Json uploaded =
        jsonOf(server->upload(mixedUpload(2025, {{"interactive_brokers", "ibkr.xml", ibkrXml}})));

    ASSERT_EQ(uploaded["requests"].size(), 1U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "corporate_action_stock_merger_required");

    const Response answered = server->postJson(
        "/api/run/answers",
        answerBody(1, "request-0001", R"({"treatment":"deferred","effectiveDate":"2024-06-03"})"));

    ASSERT_EQ(answered.result_int(), 200U);

    const Json result = jsonOf(answered);

    EXPECT_EQ(result["runRevision"], 2);
    EXPECT_TRUE(result["requests"].empty());
    EXPECT_EQ(result["reports"]["capitalGains"]["status"], "generated");

    const Response download = server->send("GET", "/api/run/reports/capitalGains/xml?revision=2");

    ASSERT_EQ(download.result_int(), 200U);

    // One list, under the ISIN of B: the purchase of A with its date, method `E` and the basis per
    // unit of B, and the sale of 4 units at EUR 140.
    EXPECT_EQ(countOf(download.body(), "<ISIN>XX0000000137</ISIN>"), 1U);
    EXPECT_EQ(countOf(download.body(), "<ISIN>XX0000000136</ISIN>"), 0U);
    EXPECT_EQ(countOf(download.body(), "<F1>2023-03-01</F1>"), 1U);
    EXPECT_EQ(countOf(download.body(), "<F2>E</F2>"), 1U);
    EXPECT_EQ(countOf(download.body(), "<F3>4.0000</F3>"), 1U);
    EXPECT_EQ(countOf(download.body(), "<F4>100.0000</F4>"), 1U);
    EXPECT_EQ(countOf(download.body(), "<F7>4.0000</F7>"), 1U);
    EXPECT_EQ(countOf(download.body(), "<F9>140.0000</F9>"), 1U);
}

TEST_F(ServerRunTest, AnIsinChangeIsAnsweredOverHttpAndMovesTheLotsToTheNewIsin) {
    // 20 units of the old ISIN bought at EUR 15 on 2023-03-01; the ISIN changes on 2024-03-04; the
    // 20 units are sold under the new ISIN on 2025-03-03 at EUR 18. The gain is 20 x (18 - 15).
    const std::string ibkrXml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="20" tradePrice="15" proceeds="-300" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1002" tradeID="1002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20250303" dateTime="20250303;100000" buySell="SELL" quantity="-20" tradePrice="18" proceeds="360" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4001" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-20" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4002" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="20" proceeds="0" value="0"/>)",
        "");

    const auto server = startServer();

    const Json uploaded =
        jsonOf(server->upload(mixedUpload(2025, {{"interactive_brokers", "ibkr.xml", ibkrXml}})));

    ASSERT_EQ(uploaded["requests"].size(), 1U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "corporate_action_isin_change_required");
    EXPECT_EQ(uploaded["requests"][0]["isinChangeCheck"]["oldIsin"], "XX0000000134");
    EXPECT_EQ(uploaded["requests"][0]["isinChangeCheck"]["newIsin"], "XX0000000135");

    // The same company, 1 : 1, on the date of the row.
    const Response answered = server->postJson(
        "/api/run/answers",
        answerBody(1,
                   "request-0001",
                   R"({"newShares":1,"oldShares":1,"effectiveDate":"2024-03-04"})"));

    ASSERT_EQ(answered.result_int(), 200U);

    const Json result = jsonOf(answered);

    EXPECT_EQ(result["runRevision"], 2);
    EXPECT_TRUE(result["requests"].empty());
    EXPECT_EQ(result["reports"]["capitalGains"]["status"], "generated");

    const Response download = server->send("GET", "/api/run/reports/capitalGains/xml?revision=2");

    ASSERT_EQ(download.result_int(), 200U);

    // One list under the new ISIN: the purchase of 2023-03-01 and the sale of 2025-03-03.
    EXPECT_EQ(countOf(download.body(), "<ISIN>XX0000000135</ISIN>"), 1U);
    EXPECT_EQ(countOf(download.body(), "<ISIN>XX0000000134</ISIN>"), 0U);
    EXPECT_EQ(countOf(download.body(), "<F1>2023-03-01</F1>"), 1U);
    EXPECT_EQ(countOf(download.body(), "<F3>20.0000</F3>"), 1U);
    EXPECT_EQ(countOf(download.body(), "<F4>15.0000</F4>"), 1U);
    EXPECT_EQ(countOf(download.body(), "<F6>2025-03-03</F6>"), 1U);
    EXPECT_EQ(countOf(download.body(), "<F9>18.0000</F9>"), 1U);
}

TEST_F(ServerRunTest, AQuantityCorrectionIsAnsweredOverHttpAndShortensTheSale) {
    // 0.99999999 units are bought and 1 unit is sold: 0.00000001 units are missing, and the source
    // digits give no proof of rounding, so the user is asked.
    constexpr std::string_view buy =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.99999999,30,-30,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sale =
        R"(2024-03-04T09:00:00Z,2024-03-04,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,36,36,,,EUR,,,,,synthetic-sell-1,,,,)";

    const auto server = startServer();

    const Json uploaded = jsonOf(server->upload(oneFileUpload(2024, {buy, sale})));

    ASSERT_EQ(uploaded["requests"].size(), 1U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "quantity_correction_required");
    EXPECT_EQ(uploaded["requests"][0]["quantityCheck"]["proposedUnits"], "0.99999999");

    const Response answered =
        server->postJson("/api/run/answers",
                         answerBody(1, "request-0001", R"({"correctedUnits":"0.99999999"})"));

    ASSERT_EQ(answered.result_int(), 200U);

    const Json result = jsonOf(answered);

    EXPECT_EQ(result["runRevision"], 2);
    EXPECT_TRUE(result["requests"].empty());
    EXPECT_EQ(result["reports"]["capitalGains"]["status"], "generated");

    // The sale row has the corrected units.
    const Response download = server->send("GET", "/api/run/reports/capitalGains/xml?revision=2");

    ASSERT_EQ(download.result_int(), 200U);
    EXPECT_EQ(countOf(download.body(), "<F7>0.99999999</F7>"), 1U);
}

TEST_F(ServerRunTest, APayerCountryIsAnsweredOverHttpAndIsTheCountryOfTheDividendRow) {
    // The export says the payer is in the US; the reference row for the ISIN says DE. The two
    // countries differ, so the user is asked.
    const std::string ibkrXml = ibkrStatement(
        "",
        "",
        R"(<CashTransaction accountId="U0000000" transactionID="2401" actionID="A9401" levelOfDetail="DETAIL" type="Dividends" assetCategory="STK" symbol="SYN" description="SYNTHETIC CASH DIVIDEND" isin="XX0000000010" currency="EUR" dateTime="20240410" settleDate="20240410" reportDate="20240410" amount="20" issuerCountryCode="US"/>)");
    const std::string payers =
        "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
        "source_revision\n"
        "XX0000000010,Payer X,TAX-X,1 Main Street,DE,,,issuer,manual,\n";
    const std::string files = R"([{"part":"file0","kind":"broker","broker":"interactive_brokers"},)"
                              R"({"part":"file1","kind":"reference"}])";

    const auto server = startServer();

    const Json uploaded = jsonOf(server->upload(
        {multipartBody(requestJson(2024, files),
                       {{"file0", "ibkr.xml", ibkrXml}, {"file1", "payers.csv", payers}}),
         contentTypeHeader()}));

    ASSERT_EQ(uploaded["requests"].size(), 1U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "income_details_required");
    ASSERT_EQ(uploaded["requests"][0]["countryCheck"]["candidates"].size(), 2U);
    EXPECT_EQ(uploaded["requests"][0]["countryCheck"]["candidates"][0]["country"], "DE");
    EXPECT_EQ(uploaded["requests"][0]["countryCheck"]["candidates"][0]["origin"], "reference");
    EXPECT_EQ(uploaded["requests"][0]["countryCheck"]["candidates"][1]["country"], "US");
    EXPECT_EQ(uploaded["requests"][0]["countryCheck"]["candidates"][1]["origin"], "export");

    const Response answered =
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"({"country":"DE"})"));

    ASSERT_EQ(answered.result_int(), 200U);

    const Json result = jsonOf(answered);

    EXPECT_EQ(result["runRevision"], 2);
    EXPECT_TRUE(result["requests"].empty());
    EXPECT_EQ(result["reports"]["dividends"]["status"], "generated");

    const Response download = server->send("GET", "/api/run/reports/dividends/xml?revision=2");

    ASSERT_EQ(download.result_int(), 200U);
    EXPECT_EQ(countOf(download.body(), "<PayerCountry>DE</PayerCountry>"), 1U);
}

TEST_F(ServerRunTest, ATreatyClaimIsAnsweredOverHttpAndIsTheReliefStatementOfTheRow) {
    // A dividend with EUR 1.50 foreign tax from a payer in SE, a country that has no treaty text in
    // the built-in reference files, so the user is asked for one. The tax column of Trade Republic
    // is negative: -1.5 is a foreign tax of EUR 1.50.
    constexpr std::string_view dividend =
        R"(2024-04-01T09:00:00Z,2024-04-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share A,XX0000000010,,,10,,-1.5,EUR,,,,,synthetic-dividend-1,,,,)";
    const std::string payers =
        "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
        "source_revision\n"
        "XX0000000010,Payer X,TAX-X,1 Main Street,SE,,,issuer,manual,\n";
    const std::string files = R"([{"part":"file0","kind":"broker","broker":"trade_republic"},)"
                              R"({"part":"file1","kind":"reference"}])";

    const auto server = startServer();

    const Json uploaded =
        jsonOf(server->upload({multipartBody(requestJson(2024, files),
                                             {{"file0", "synthetic.csv", csvOfRows({dividend})},
                                              {"file1", "payers.csv", payers}}),
                               contentTypeHeader()}));

    ASSERT_EQ(uploaded["requests"].size(), 1U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "income_details_required");
    EXPECT_EQ(uploaded["requests"][0]["treatyCheck"]["country"], "SE");
    EXPECT_EQ(uploaded["requests"][0]["treatyCheck"]["isins"], Json::parse(R"(["XX0000000010"])"));

    const Response answered =
        server->postJson("/api/run/answers",
                         answerBody(1, "request-0001", R"json({"statement":"Art. 10(2)"})json"));

    ASSERT_EQ(answered.result_int(), 200U);

    const Json result = jsonOf(answered);

    EXPECT_EQ(result["runRevision"], 2);
    EXPECT_TRUE(result["requests"].empty());

    const Response download = server->send("GET", "/api/run/reports/dividends/xml?revision=2");

    ASSERT_EQ(download.result_int(), 200U);
    EXPECT_EQ(countOf(download.body(), "<ReliefStatement>Art. 10(2)</ReliefStatement>"), 1U);
}
} // namespace
