// The run API through a real server: upload, result, answers, deleting answers and the XML
// downloads. The inputs are the synthetic fixtures of `tests/test_data/pipeline` and `cli`; the
// expected values are those of `tests/test_data/pipeline/README.md` and the golden files.

#include "server_test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;
using namespace taxbroker::servertest;

using Json = nlohmann::json;

// ---- Fixtures

/// A committed file under `tests/test_data`, read byte for byte.
std::string readTestData(std::string_view aFolder, std::string_view aName) {
    const auto path =
        std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / aFolder / aName;
    std::ifstream input{path, std::ios::binary};

    if (!input.is_open())
    {
        throw std::runtime_error{"Unable to open the test file " + std::string{aName}};
    }

    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// All rows are synthetic. The header is the one of the Trade Republic export.
constexpr std::string_view kTradeRepublicHeader =
    "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,"
    "currency,original_amount,original_currency,fx_rate,description,transaction_id,"
    "counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

/// A Trade Republic file made of the header and the given rows.
std::string csvOfRows(const std::vector<std::string_view>& aRows) {
    std::string content{kTradeRepublicHeader};

    for (const auto row : aRows)
    {
        content += row;
        content += '\n';
    }

    return content;
}

// The loss question: 10 units bought at EUR 30 and sold at EUR 20 in 2025, with no purchase within
// 30 days of the sale. The loss is 10 x 20 - 10 x 30 = EUR 100.
constexpr std::string_view kLossBuy =
    R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)";
constexpr std::string_view kLossSell =
    R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)";

/// The taxpayer of the golden files.
constexpr std::string_view kTaxpayerJson = R"({
    "taxNumber": "12345678",
    "isResident": true,
    "name": "Synthetic Taxpayer",
    "address": "Synthetic Street 1",
    "city": "Synthetic City",
    "postCode": "1000",
    "postName": "Synthetic City",
    "birthDate": "1990-01-31",
    "email": "taxpayer@example.com",
    "phoneNumber": "+38600000000"
})";

/// One file of an upload: the part name, the file name the browser sends and the bytes.
struct UploadFile {
    std::string mPart;
    std::string mFileName;
    std::string mContent;
};

constexpr std::string_view kBoundary = "tbrBoundary7MA4YWxkTrZu0gW";

/// The `multipart/form-data` body: the part `request` first, then the files.
std::string multipartBody(const std::string& aRequestJson, const std::vector<UploadFile>& aFiles) {
    std::string body = "--" + std::string{kBoundary} +
                       "\r\nContent-Disposition: form-data; name=\"request\"\r\n"
                       "Content-Type: application/json\r\n\r\n" +
                       aRequestJson + "\r\n";

    for (const UploadFile& file : aFiles)
    {
        body += "--" + std::string{kBoundary} + "\r\nContent-Disposition: form-data; name=\"" +
                file.mPart + "\"; filename=\"" + file.mFileName +
                "\"\r\nContent-Type: text/csv\r\n\r\n" + file.mContent + "\r\n";
    }

    body += "--" + std::string{kBoundary} + "--\r\n";

    return body;
}

/// The request JSON for the year with the synthetic taxpayer and the given `files` list text.
std::string requestJson(int aYear, const std::string& aFilesList, bool aDeveloperMode = false) {
    return R"({"reportingYear":)" + std::to_string(aYear) +
           R"(,"filingType":"original","developerMode":)" + (aDeveloperMode ? "true" : "false") +
           R"(,"taxpayer":)" + std::string{kTaxpayerJson} + R"(,"files":)" + aFilesList + "}";
}

/// The `files` list for one broker file, `file0`.
constexpr std::string_view kOneBrokerFile =
    R"([{"part":"file0","kind":"broker","broker":"trade_republic"}])";

/// The `files` list for two broker files and the rate file.
constexpr std::string_view kTwoBrokerFilesAndRates =
    R"([{"part":"file0","kind":"broker","broker":"trade_republic"},)"
    R"({"part":"file1","kind":"broker","broker":"trade_republic"},)"
    R"({"part":"file2","kind":"ecb_rates"}])";

/// The pipeline upload: both exports and the made-up rate file.
struct PipelineUpload {
    std::string mBody;
    std::string mContentType;
};

std::string contentTypeHeader() {
    return "multipart/form-data; boundary=" + std::string{kBoundary};
}

PipelineUpload pipelineUpload(int aYear, bool aReversed) {
    const std::string first = readTestData("pipeline", "export_2023_2024.csv");
    const std::string second = readTestData("pipeline", "export_2024_2025.csv");

    return {multipartBody(requestJson(aYear, std::string{kTwoBrokerFilesAndRates}),
                          {{"file0", "export_a.csv", aReversed ? second : first},
                           {"file1", "export_b.csv", aReversed ? first : second},
                           {"file2", "rates.csv", readTestData("cli", "report_ecb_rates.csv")}}),
            contentTypeHeader()};
}

/// An upload of one Trade Republic file made of the given rows.
PipelineUpload oneFileUpload(int aYear, const std::vector<std::string_view>& aRows) {
    return {multipartBody(requestJson(aYear, std::string{kOneBrokerFile}),
                          {{"file0", "synthetic.csv", csvOfRows(aRows)}}),
            contentTypeHeader()};
}

// ---- The server and its client

/// A server with a logged-in session. Every request goes over a new connection and is checked
/// for host paths (R12): no response body of the API may hold a folder of the machine.
class LoggedInServer {
  public:
    LoggedInServer(const std::filesystem::path& aFolder, std::size_t aMaxFileBytes)
        : mLaunchFile(aFolder / "run" / "taxbroker" / "launch.html"),
          mServer(
              envOf({{"XDG_STATE_HOME", (aFolder / "state").string()},
                     {"XDG_RUNTIME_DIR", (aFolder / "run").string()},
                     {"HOME", aFolder.string()}}),
              [](const std::filesystem::path&) { return true; },
              aMaxFileBytes) {
        const std::string link = tokenIn(readAll(mLaunchFile));
        Client client(mServer.port());

        client.send(loginRequest(mServer.port(), link));

        const std::optional<Response> login = client.readResponse();

        if (!login.has_value() || login->result_int() != 204U)
        {
            throw std::runtime_error{"The login failed."};
        }

        const std::string cookie = std::string((*login)[http::field::set_cookie]);
        const std::string prefix = "tbr_session_" + std::to_string(mServer.port()) + "=";

        mSession = cookie.substr(prefix.size(), 64);
    }

    [[nodiscard]] std::uint16_t port() const {
        return mServer.port();
    }

    /// One request with the session, the Origin and the client header.
    Response send(const std::string& aMethod,
                  const std::string& aTarget,
                  const std::string& aContentType = {},
                  const std::string& aBody = {}) {
        return sendWith(aMethod, aTarget, aContentType, aBody, true, true);
    }

    /// Like `send`, with the session cookie and the client header only if asked.
    Response sendWith(const std::string& aMethod,
                      const std::string& aTarget,
                      const std::string& aContentType,
                      const std::string& aBody,
                      bool aWithSession,
                      bool aWithClientHeader) {
        std::string request = aMethod + " " + aTarget +
                              " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port()) +
                              "\r\nOrigin: http://127.0.0.1:" + std::to_string(port()) + "\r\n";

        if (aWithClientHeader)
        {
            request += "X-TaxBroker: 1\r\n";
        }

        if (aWithSession)
        {
            request += "Cookie: tbr_session_" + std::to_string(port()) + "=" + mSession + "\r\n";
        }

        if (!aContentType.empty())
        {
            request += "Content-Type: " + aContentType + "\r\n";
        }

        request += "Content-Length: " + std::to_string(aBody.size()) + "\r\n\r\n" + aBody;

        Client client(port());

        client.send(request);

        std::optional<Response> response = client.readResponse();

        if (!response.has_value())
        {
            throw std::runtime_error{"No response to " + aMethod + " " + aTarget};
        }

        // R12: no folder of the machine in any body.
        for (const std::string_view folder : {"/home/", "/tmp/", "/workspace/", "C:\\"})
        {
            EXPECT_EQ(response->body().find(folder), std::string::npos) << aTarget;
        }

        return std::move(*response);
    }

    Response upload(const PipelineUpload& aUpload) {
        return send("POST", "/api/run", aUpload.mContentType, aUpload.mBody);
    }

    Response postJson(const std::string& aTarget, const std::string& aBody) {
        return send("POST", aTarget, "application/json", aBody);
    }

  private:
    std::filesystem::path mLaunchFile;
    RunningServer mServer;
    std::string mSession; ///< Set by the constructor.
};

/// The JSON of a response body.
Json jsonOf(const Response& aResponse) {
    return Json::parse(aResponse.body());
}

/// The text of a header of the response.
std::string headerOf(const Response& aResponse, std::string_view aName) {
    return std::string(aResponse.base()[aName]);
}

class ServerRunTest : public ::testing::Test {
  protected:
    void SetUp() override {
        mFolder = std::filesystem::temp_directory_path() /
                  (std::string("tbr_server_run_") +
                   ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(mFolder);
        std::filesystem::create_directories(mFolder);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(mFolder, ignored);
    }

    /// A server with a session; its state and launch folders are in a subfolder of the test
    /// folder, so two servers of one test do not meet.
    std::unique_ptr<LoggedInServer> startServer(std::size_t aMaxFileBytes = kMaxInputFileBytes,
                                                const std::string& aSubfolder = "main") {
        return std::make_unique<LoggedInServer>(mFolder / aSubfolder, aMaxFileBytes);
    }

    std::filesystem::path mFolder;
};

/// The answer call body for one answer.
std::string
answerBody(std::uint64_t aRevision, const std::string& aRequestId, const std::string& aAnswerJson) {
    return R"({"runRevision":)" + std::to_string(aRevision) + R"(,"answers":[{"requestId":")" +
           aRequestId + R"(","answer":)" + aAnswerJson + "}]}";
}

/// The delete call body for the given decision IDs.
std::string deleteBody(std::uint64_t aRevision, const std::string& aIdsList) {
    return R"({"runRevision":)" + std::to_string(aRevision) + R"(,"decisionIds":)" + aIdsList + "}";
}

// ---- Success and downloads

TEST_F(ServerRunTest, TwoExportsGiveTheFourGoldenFormsFor2024) {
    const auto server = startServer();

    const Response uploaded = server->upload(pipelineUpload(2024, false));

    ASSERT_EQ(uploaded.result_int(), 200U);
    EXPECT_EQ(headerOf(uploaded, "Content-Type"), "application/json");

    const Json first = jsonOf(uploaded);

    EXPECT_EQ(first["schemaVersion"], 2);
    EXPECT_EQ(first["runRevision"], 1);
    EXPECT_EQ(first["status"], "completed_with_warnings");

    // The deposit interest total is the one open request.
    ASSERT_EQ(first["requests"].size(), 1U);
    EXPECT_EQ(first["requests"][0]["id"], "request-0001");
    EXPECT_EQ(first["requests"][0]["type"], "deposit_interest_total_required");

    const Response answered =
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("above_limit")"));

    ASSERT_EQ(answered.result_int(), 200U);

    const Json second = jsonOf(answered);

    EXPECT_EQ(second["runRevision"], 2);
    EXPECT_TRUE(second["requests"].empty());

    const Response capital = server->send("GET", "/api/run/reports/capitalGains/xml?revision=2");
    const Response dividends = server->send("GET", "/api/run/reports/dividends/xml?revision=2");
    const Response deposit = server->send("GET", "/api/run/reports/bankInterest/xml?revision=2");
    const Response other = server->send("GET", "/api/run/reports/otherInterest/xml?revision=2");

    EXPECT_EQ(capital.body(), readTestData("pipeline", "Doh-KDVP_2024.golden.xml"));
    EXPECT_EQ(dividends.body(), readTestData("pipeline", "Doh-Div_2024.golden.xml"));
    EXPECT_EQ(deposit.body(), readTestData("pipeline", "Doh-DHO_2024.golden.xml"));
    EXPECT_EQ(other.body(), readTestData("pipeline", "Doh-Obr_2024.golden.xml"));

    EXPECT_EQ(headerOf(capital, "Content-Type"), "application/xml; charset=utf-8");
    EXPECT_EQ(headerOf(capital, "Content-Disposition"),
              "attachment; filename=\"Doh-KDVP_2024.xml\"");
    EXPECT_EQ(headerOf(dividends, "Content-Disposition"),
              "attachment; filename=\"Doh-Div_2024.xml\"");
    EXPECT_EQ(headerOf(deposit, "Content-Disposition"),
              "attachment; filename=\"Doh-DHO_2024.xml\"");
    EXPECT_EQ(headerOf(other, "Content-Disposition"), "attachment; filename=\"Doh-Obr_2024.xml\"");
}

TEST_F(ServerRunTest, TheSameExportsGiveTheGoldenDohKdvpFor2025) {
    const auto server = startServer();

    const Response uploaded = server->upload(pipelineUpload(2025, false));

    ASSERT_EQ(uploaded.result_int(), 200U);

    const Json result = jsonOf(uploaded);

    EXPECT_EQ(result["status"], "success");
    EXPECT_EQ(result["reports"]["capitalGains"]["status"], "generated");
    EXPECT_EQ(result["reports"]["dividends"]["status"], "no_data");
    EXPECT_EQ(result["reports"]["bankInterest"]["status"], "no_data");
    EXPECT_EQ(result["reports"]["otherInterest"]["status"], "no_data");

    const Response capital = server->send("GET", "/api/run/reports/capitalGains/xml?revision=1");

    ASSERT_EQ(capital.result_int(), 200U);
    EXPECT_EQ(capital.body(), readTestData("pipeline", "Doh-KDVP_2025.golden.xml"));

    // A form with no data has no file.
    EXPECT_EQ(server->send("GET", "/api/run/reports/dividends/xml?revision=1").result_int(), 404U);
    EXPECT_EQ(server->send("GET", "/api/run/reports/bankInterest/xml?revision=1").result_int(),
              404U);
    EXPECT_EQ(server->send("GET", "/api/run/reports/otherInterest/xml?revision=1").result_int(),
              404U);
}

TEST_F(ServerRunTest, TheResultIsNotFoundBeforeAnUploadAndEqualsTheUploadResponseAfterIt) {
    const auto server = startServer();

    const Response before = server->send("GET", "/api/run");

    EXPECT_EQ(before.result_int(), 404U);
    EXPECT_EQ(before.body(), R"({"error":"no_run"})");

    const Response uploaded = server->upload(pipelineUpload(2024, false));
    const Response after = server->send("GET", "/api/run");

    ASSERT_EQ(uploaded.result_int(), 200U);
    ASSERT_EQ(after.result_int(), 200U);
    EXPECT_EQ(after.body(), uploaded.body());
}

TEST_F(ServerRunTest, EitherFileOrderGivesTheSameFourDocuments) {
    const auto server = startServer();

    ASSERT_EQ(server->upload(pipelineUpload(2024, false)).result_int(), 200U);

    const Response capitalA = server->send("GET", "/api/run/reports/capitalGains/xml?revision=1");
    const Response dividendsA = server->send("GET", "/api/run/reports/dividends/xml?revision=1");
    const Response depositA = server->send("GET", "/api/run/reports/bankInterest/xml?revision=1");
    const Response otherA = server->send("GET", "/api/run/reports/otherInterest/xml?revision=1");

    // The same files in the other order: a second upload, revision 2.
    ASSERT_EQ(server->upload(pipelineUpload(2024, true)).result_int(), 200U);

    EXPECT_EQ(server->send("GET", "/api/run/reports/capitalGains/xml?revision=2").body(),
              capitalA.body());
    EXPECT_EQ(server->send("GET", "/api/run/reports/dividends/xml?revision=2").body(),
              dividendsA.body());
    EXPECT_EQ(server->send("GET", "/api/run/reports/bankInterest/xml?revision=2").body(),
              depositA.body());
    EXPECT_EQ(server->send("GET", "/api/run/reports/otherInterest/xml?revision=2").body(),
              otherA.body());
}

// ---- Partial success and failure

TEST_F(ServerRunTest, AChangedOverlappingSaleFailsOnlyDohKdvp) {
    // The two files hold the transaction p-sell with another price: EUR 30 and EUR 31.
    constexpr std::string_view buy =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,15,10,-150,,,EUR,,,,,p-buy,,,,)";
    constexpr std::string_view sale =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,30,450,,,EUR,,,,,p-sell,,,,)";
    constexpr std::string_view changedSale =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,31,465,,,EUR,,,,,p-sell,,,,)";
    constexpr std::string_view dividend =
        R"(2024-03-14T09:00:00Z,2024-03-14,DEFAULT,CASH,DIVIDEND,STOCK,Apple Inc,US0378331005,,,10.00,,,EUR,12.50,USD,0.800000,,p-div-apple,,,,)";

    const auto server = startServer();
    const std::string files = R"([{"part":"file0","kind":"broker","broker":"trade_republic"},)"
                              R"({"part":"file1","kind":"broker","broker":"trade_republic"}])";

    const Response uploaded =
        server->upload({multipartBody(requestJson(2024, files),
                                      {{"file0", "first.csv", csvOfRows({buy, sale, dividend})},
                                       {"file1", "second.csv", csvOfRows({changedSale})}}),
                        contentTypeHeader()});

    ASSERT_EQ(uploaded.result_int(), 200U);

    const Json result = jsonOf(uploaded);

    EXPECT_EQ(result["status"], "partial_success");
    EXPECT_EQ(result["reports"]["capitalGains"]["status"], "failed");
    EXPECT_EQ(result["reports"]["dividends"]["status"], "generated");

    EXPECT_EQ(server->send("GET", "/api/run/reports/capitalGains/xml?revision=1").result_int(),
              404U);
    EXPECT_EQ(server->send("GET", "/api/run/reports/dividends/xml?revision=1").result_int(), 200U);
}

TEST_F(ServerRunTest, ABrokenRecordFailsAllFourFormsAndNothingCanBeDownloaded) {
    // 22 fields: mcc_code is missing.
    constexpr std::string_view shortRecord =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,)";

    const auto server = startServer();

    const Response uploaded = server->upload(oneFileUpload(2024, {shortRecord}));

    ASSERT_EQ(uploaded.result_int(), 200U);

    const Json result = jsonOf(uploaded);

    EXPECT_EQ(result["status"], "failed");
    EXPECT_EQ(result["reports"]["capitalGains"]["status"], "failed");
    EXPECT_EQ(result["reports"]["dividends"]["status"], "failed");
    EXPECT_EQ(result["reports"]["bankInterest"]["status"], "failed");
    EXPECT_EQ(result["reports"]["otherInterest"]["status"], "failed");

    EXPECT_EQ(server->send("GET", "/api/run/reports/capitalGains/xml?revision=1").result_int(),
              404U);
    EXPECT_EQ(server->send("GET", "/api/run/reports/dividends/xml?revision=1").result_int(), 404U);
    EXPECT_EQ(server->send("GET", "/api/run/reports/bankInterest/xml?revision=1").result_int(),
              404U);
    EXPECT_EQ(server->send("GET", "/api/run/reports/otherInterest/xml?revision=1").result_int(),
              404U);
}

TEST_F(ServerRunTest, FailingDohDivLeavesDohKdvpDownloadable) {
    // The share sale makes Doh-KDVP. XX0000000001 pays a dividend in RUB, which has no official
    // rate after March 2022; XX0000000002 pays EUR 10.00.
    constexpr std::string_view buy =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000009,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sale =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000009,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view rubDividend =
        R"(2024-04-01T09:00:00Z,2024-04-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share A,XX0000000001,,,9,,,EUR,1000,RUB,0.009000,,synthetic-dividend-1,,,,)";
    constexpr std::string_view eurDividend =
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share B,XX0000000002,,,10,,,EUR,,,,,synthetic-dividend-2,,,,)";

    const auto server = startServer();

    const Json uploaded =
        jsonOf(server->upload(oneFileUpload(2024, {buy, sale, rubDividend, eurDividend})));

    ASSERT_EQ(uploaded["requests"].size(), 1U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "dividends_failure_resolution_required");
    EXPECT_EQ(uploaded["status"], "needs_input");

    const Response answered =
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("fail_report")"));

    ASSERT_EQ(answered.result_int(), 200U);

    const Json result = jsonOf(answered);

    EXPECT_EQ(result["status"], "partial_success");
    EXPECT_EQ(result["reports"]["dividends"]["status"], "failed");
    EXPECT_EQ(result["reports"]["capitalGains"]["status"], "generated");

    EXPECT_EQ(server->send("GET", "/api/run/reports/dividends/xml?revision=2").result_int(), 404U);
    EXPECT_EQ(server->send("GET", "/api/run/reports/capitalGains/xml?revision=2").result_int(),
              200U);
}

TEST_F(ServerRunTest, CapitalAndDohObrExclusionsAreIndependent) {
    constexpr std::string_view buy1 =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view buy2 =
        R"(2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,20,20,-400,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view sale =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,30,450,,,EUR,,,,,synthetic-sell,,,,)";
    // A sale of 5 units of XX0000000002 that was never bought: the history is incomplete.
    constexpr std::string_view saleWithoutPurchase =
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,TRADING,SELL,STOCK,Other Share,XX0000000002,-5,10,50,,,EUR,,,,,synthetic-sale-2,,,,)";
    // A coupon with a RUB original amount: the ECB has no RUB rate for 2024, so Doh-Obr fails for
    // XX0000000003.
    constexpr std::string_view failingCoupon =
        R"(2024-08-01T10:00:00Z,2024-08-01,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond A,XX0000000003,,,1.80,,,EUR,200.00,RUB,0.009000,Synthetic bond interest,synthetic-coupon-1,,,,)";
    constexpr std::string_view goodCoupon =
        R"(2024-03-10T10:00:00Z,2024-03-10,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond B,XS0000000002,,,10.00,,,EUR,,,,Interest payment,synthetic-coupon-2,,,,)";

    const auto server = startServer();

    const Json uploaded = jsonOf(server->upload(
        oneFileUpload(2024, {buy1, buy2, sale, saleWithoutPurchase, failingCoupon, goodCoupon})));

    // Two failure questions, capital gains first and Doh-Obr second.
    ASSERT_EQ(uploaded["requests"].size(), 2U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "capital_gains_failure_resolution_required");
    EXPECT_EQ(uploaded["requests"][1]["type"], "other_interest_failure_resolution_required");
    EXPECT_EQ(uploaded["requests"][1]["id"], "request-0002");

    // Exclude the capital failure only: the Doh-Obr question is still open.
    const Response firstAnswer =
        server->postJson("/api/run/answers",
                         answerBody(1, "request-0001", R"("exclude_failed_instruments")"));

    ASSERT_EQ(firstAnswer.result_int(), 200U);

    const Json afterFirst = jsonOf(firstAnswer);

    EXPECT_EQ(afterFirst["reports"]["capitalGains"]["status"], "generated");
    EXPECT_EQ(afterFirst["reports"]["capitalGains"]["failureResolution"],
              "exclude_failed_instruments");
    EXPECT_EQ(afterFirst["reports"]["capitalGains"]["excludedInvestments"][0]["isin"],
              "XX0000000002");
    EXPECT_EQ(afterFirst["reports"]["otherInterest"]["status"], "needs_input");
    ASSERT_EQ(afterFirst["requests"].size(), 1U);
    EXPECT_EQ(afterFirst["requests"][0]["type"], "other_interest_failure_resolution_required");

    // Fail the Doh-Obr report: only it fails, the capital exclusion stays listed.
    const std::string otherRequestId = afterFirst["requests"][0]["id"];
    const Response secondAnswer =
        server->postJson("/api/run/answers", answerBody(2, otherRequestId, R"("fail_report")"));

    ASSERT_EQ(secondAnswer.result_int(), 200U);

    const Json afterSecond = jsonOf(secondAnswer);

    EXPECT_EQ(afterSecond["reports"]["otherInterest"]["status"], "failed");
    EXPECT_EQ(afterSecond["reports"]["capitalGains"]["status"], "generated");
    EXPECT_EQ(afterSecond["reports"]["capitalGains"]["failureResolution"],
              "exclude_failed_instruments");
    EXPECT_EQ(afterSecond["reports"]["capitalGains"]["excludedInvestments"][0]["isin"],
              "XX0000000002");
}

// ---- Answers, stale answers and restart

TEST_F(ServerRunTest, AStaleAnswerIsRejectedAndChangesNothing) {
    const auto server = startServer();

    ASSERT_EQ(server->upload(pipelineUpload(2024, false)).result_int(), 200U);
    ASSERT_EQ(
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("above_limit")"))
            .result_int(),
        200U);

    // The same answer again, still for revision 1, while revision 2 is current.
    const Response stale =
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("above_limit")"));

    ASSERT_EQ(stale.result_int(), 422U);
    EXPECT_EQ(
        jsonOf(stale),
        Json::parse(R"({"rejections":[{"index":0,"messageKey":"answer_rejected.stale_revision",)"
                    R"("messageParams":{"requestId":"request-0001"}}]})"));

    EXPECT_EQ(jsonOf(server->send("GET", "/api/run"))["runRevision"], 2);
}

TEST_F(ServerRunTest, AnUnknownRequestRejectsTheWholeCall) {
    const auto server = startServer();

    ASSERT_EQ(server->upload(pipelineUpload(2024, false)).result_int(), 200U);

    const Response response = server->postJson(
        "/api/run/answers",
        R"({"runRevision":1,"answers":[{"requestId":"request-0001","answer":"above_limit"},)"
        R"({"requestId":"request-0099","answer":"above_limit"}]})");

    ASSERT_EQ(response.result_int(), 422U);
    EXPECT_EQ(
        jsonOf(response),
        Json::parse(R"({"rejections":[{"index":1,"messageKey":"answer_rejected.unknown_request",)"
                    R"("messageParams":{"requestId":"request-0099"}}]})"));

    // The first answer was not applied.
    const Json current = jsonOf(server->send("GET", "/api/run"));

    EXPECT_EQ(current["runRevision"], 1);
    EXPECT_EQ(current["requests"].size(), 1U);
}

TEST_F(ServerRunTest, TheSameRequestAnsweredTwiceInOneCallIsRejected) {
    const auto server = startServer();

    ASSERT_EQ(server->upload(pipelineUpload(2024, false)).result_int(), 200U);

    const Response response = server->postJson(
        "/api/run/answers",
        R"({"runRevision":1,"answers":[{"requestId":"request-0001","answer":"above_limit"},)"
        R"({"requestId":"request-0001","answer":"not_above_limit"}]})");

    ASSERT_EQ(response.result_int(), 422U);
    EXPECT_EQ(
        jsonOf(response),
        Json::parse(R"({"rejections":[{"index":1,"messageKey":"answer_rejected.duplicate_answer",)"
                    R"("messageParams":{"requestId":"request-0001"}}]})"));
}

TEST_F(ServerRunTest, AnAnswerThatDoesNotFitTheQuestionNeverReachesTheSession) {
    const auto server = startServer();

    ASSERT_EQ(server->upload(pipelineUpload(2024, false)).result_int(), 200U);

    // "skip" is an answer of the payer question, not of the deposit question.
    const Response response =
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("skip")"));

    ASSERT_EQ(response.result_int(), 422U);
    EXPECT_EQ(jsonOf(response),
              Json::parse(
                  R"({"rejections":[{"index":0,"messageKey":"answer_rejected.answer_does_not_fit",)"
                  R"("messageParams":{"requestId":"request-0001"}}]})"));
    EXPECT_EQ(jsonOf(server->send("GET", "/api/run"))["runRevision"], 1);
}

TEST_F(ServerRunTest, ARatioOfZeroPassesTheReaderAndTheSessionRejectsIt) {
    const auto server = startServer();
    const std::string files = std::string{kOneBrokerFile};

    const Json uploaded = jsonOf(server->upload(
        {multipartBody(
             requestJson(2025, files),
             {{"file0", "split_question.csv", readTestData("cli", "split_question.csv")}}),
         contentTypeHeader()}));

    ASSERT_EQ(uploaded["requests"].size(), 1U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "corporate_action_ratio_required");

    const Response response = server->postJson(
        "/api/run/answers",
        answerBody(
            1,
            "request-0001",
            R"({"newShares":0,"oldShares":1,"effectiveDate":"2023-01-15","confirmedUnits":"2"})"));

    ASSERT_EQ(response.result_int(), 422U);
    EXPECT_EQ(jsonOf(response),
              Json::parse(
                  R"({"rejections":[{"index":0,"messageKey":"answer_rejected.ratio_not_positive",)"
                  R"("messageParams":{"requestId":"request-0001","field":"newShares"}}]})"));
}

TEST_F(ServerRunTest, ARatioAnswerIsAcceptedAndRecorded) {
    const auto server = startServer();

    ASSERT_EQ(server
                  ->upload({multipartBody(requestJson(2025, std::string{kOneBrokerFile}),
                                          {{"file0",
                                            "split_question.csv",
                                            readTestData("cli", "split_question.csv")}}),
                            contentTypeHeader()})
                  .result_int(),
              200U);

    // 1 unit bought in 2022 becomes 2 units with the ratio 2 : 1.
    const Response response = server->postJson(
        "/api/run/answers",
        answerBody(
            1,
            "request-0001",
            R"({"newShares":2,"oldShares":1,"effectiveDate":"2023-01-15","confirmedUnits":"2"})"));

    ASSERT_EQ(response.result_int(), 200U);

    const Json result = jsonOf(response);

    EXPECT_EQ(result["runRevision"], 2);
    EXPECT_TRUE(result["requests"].empty());
    ASSERT_EQ(result["decisions"].size(), 1U);
    EXPECT_EQ(result["decisions"][0]["type"], "corporate_action_ratio_required");
    EXPECT_EQ(result["reports"]["capitalGains"]["status"], "generated");
}

TEST_F(ServerRunTest, APayerChoiceIsAnsweredOverHttp) {
    // One dividend of XX0000000010 and a reference file with two payers for that ISIN.
    constexpr std::string_view dividend =
        R"(2024-04-01T09:00:00Z,2024-04-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share A,XX0000000010,,,10,,,EUR,,,,,synthetic-dividend-1,,,,)";
    const std::string payers =
        "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
        "source_revision\n"
        "XX0000000010,Payer X,TAX-X,1 Main Street,DE,,,issuer,manual,\n"
        "XX0000000010,Payer Y,TAX-Y,2 Main Street,FR,,,issuer,manual,\n";
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

    // The user picks the first row, Payer X.
    const Response answered =
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"({"option":0})"));

    ASSERT_EQ(answered.result_int(), 200U);

    const Json result = jsonOf(answered);

    EXPECT_EQ(result["runRevision"], 2);
    EXPECT_TRUE(result["requests"].empty());
    ASSERT_EQ(result["decisions"].size(), 1U);
    EXPECT_EQ(result["decisions"][0]["type"], "income_details_required");
    EXPECT_EQ(result["reports"]["dividends"]["status"], "generated");
}

TEST_F(ServerRunTest, ADeveloperExportIsConfirmedOverHttpAndDownloadedAsUnsafe) {
    // XX0000000001: the 2024 sale of 0.5 units is 0.15 units short, so the history is incomplete.
    const auto server = startServer();

    const Json uploaded = jsonOf(server->upload(
        {multipartBody(
             requestJson(2024, std::string{kOneBrokerFile}, true),
             {{"file0", "developer_export.csv", readTestData("cli", "developer_export.csv")}}),
         contentTypeHeader()}));

    ASSERT_EQ(uploaded["requests"].size(), 1U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "capital_gains_failure_resolution_required");

    // The choice, then the three confirmations; each answer is a whole new revision.
    ASSERT_EQ(
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("developer_export")"))
            .result_int(),
        200U);

    Json current = jsonOf(server->send("GET", "/api/run"));

    for (std::uint64_t step = 1; step <= 3; ++step)
    {
        ASSERT_EQ(current["requests"].size(), 1U) << step;
        EXPECT_EQ(current["requests"][0]["type"], "developer_export_confirmation_required") << step;
        EXPECT_EQ(current["runRevision"], step + 1) << step;

        const std::string requestId = current["requests"][0]["id"];
        const Response confirmed =
            server->postJson("/api/run/answers", answerBody(step + 1, requestId, R"("confirm")"));

        ASSERT_EQ(confirmed.result_int(), 200U) << step;
        current = jsonOf(confirmed);
    }

    EXPECT_EQ(current["runRevision"], 5);
    EXPECT_TRUE(current["requests"].empty());
    EXPECT_EQ(current["reports"]["capitalGains"]["status"], "generated");
    EXPECT_EQ(current["reports"]["capitalGains"]["unsafe"], true);

    const Response download = server->send("GET", "/api/run/reports/capitalGains/xml?revision=5");

    ASSERT_EQ(download.result_int(), 200U);
    EXPECT_EQ(headerOf(download, "Content-Disposition"),
              "attachment; filename=\"Doh-KDVP_2024_UNSAFE.xml\"");
}

TEST_F(ServerRunTest, AnInterestTreatyClaimIsAnsweredOverHttp) {
    // Before 6 December 2023 this Trade Republic interest is cash interest for Doh-Obr. EUR 0.20
    // foreign tax was withheld, so the treaty question is asked.
    constexpr std::string_view interest =
        R"(2023-11-01T10:00:00Z,2023-11-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,1.23,,-0.20,EUR,,,,Synthetic broker interest,synthetic-interest-1,,,,)";

    const auto server = startServer();

    const Json uploaded = jsonOf(server->upload(oneFileUpload(2023, {interest})));

    ASSERT_EQ(uploaded["requests"].size(), 1U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "income_details_required");

    const Response answered =
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("claim")"));

    ASSERT_EQ(answered.result_int(), 200U);

    const Json result = jsonOf(answered);

    EXPECT_EQ(result["runRevision"], 2);
    EXPECT_TRUE(result["requests"].empty());
    ASSERT_EQ(result["decisions"].size(), 1U);
    EXPECT_EQ(result["decisions"][0]["type"], "income_details_required");
    EXPECT_EQ(result["reports"]["otherInterest"]["status"], "generated");
}

TEST_F(ServerRunTest, ABondTypeIsAnsweredOverHttp) {
    // A bond bought in 2025 (1000 units), two EUR coupons, then its maturity: the type of the bond
    // decides how the coupons are reported.
    constexpr std::string_view buy =
        R"(2025-02-03T09:00:00Z,2025-02-03,DEFAULT,TRADING,BUY,BOND,Synthetic Bond,XX0000000003,1000,1,-1000,,,EUR,,,,,synthetic-bond-buy-1,,,,)";
    constexpr std::string_view coupon1 =
        R"(2025-02-14T16:00:00Z,2025-02-14,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond,XX0000000003,,,18,,,EUR,,,,Synthetic bond interest,synthetic-coupon-1,,,,)";
    constexpr std::string_view coupon2 =
        R"(2025-08-14T15:00:00Z,2025-08-14,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond,XX0000000003,,,18.40,,,EUR,,,,Synthetic bond interest,synthetic-coupon-2,,,,)";
    constexpr std::string_view maturity =
        R"(2025-08-14T15:01:00Z,2025-08-14,DEFAULT,CORPORATE_ACTION,FINAL_MATURITY,BOND,Synthetic Bond,XX0000000003,-1000,,,,,,,,,FINAL_MATURITY XX0000000003,synthetic-bond-maturity-1,,,,)";

    const auto server = startServer();

    const Json uploaded =
        jsonOf(server->upload(oneFileUpload(2025, {buy, coupon1, coupon2, maturity})));

    ASSERT_EQ(uploaded["requests"].size(), 1U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "income_details_required");
    EXPECT_EQ(uploaded["reports"]["otherInterest"]["status"], "needs_input");

    const Response answered =
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("ordinary")"));

    ASSERT_EQ(answered.result_int(), 200U);

    const Json result = jsonOf(answered);

    EXPECT_TRUE(result["requests"].empty());
    EXPECT_EQ(result["reports"]["otherInterest"]["status"], "generated");
}

TEST_F(ServerRunTest, AnIncomeClassificationIsAnsweredOverHttp) {
    // An interest payment of an instrument with the class STOCK has an unclear type.
    constexpr std::string_view interest =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,CASH,INTEREST_PAYMENT,STOCK,Synthetic Payer,XX0000000001,,,12,,,EUR,,,,Synthetic interest,synthetic-interest-1,,,,)";

    const auto server = startServer();

    const Json uploaded = jsonOf(server->upload(oneFileUpload(2024, {interest})));

    ASSERT_EQ(uploaded["requests"].size(), 1U);
    EXPECT_EQ(uploaded["requests"][0]["type"], "tax_classification_required");

    const Response answered = server->postJson(
        "/api/run/answers",
        answerBody(1, "request-0001", R"({"choice":"other_interest","subtype":"broker_cash"})"));

    ASSERT_EQ(answered.result_int(), 200U);

    const Json result = jsonOf(answered);

    EXPECT_TRUE(result["requests"].empty());
    EXPECT_EQ(result["reports"]["otherInterest"]["status"], "generated");
}

TEST_F(ServerRunTest, ARestartKeepsTheAnswerAndMakesTheEarlierRevisionStale) {
    const auto server = startServer();
    const PipelineUpload upload = oneFileUpload(2025, {kLossBuy, kLossSell});

    const Json first = jsonOf(server->upload(upload));

    ASSERT_EQ(first["requests"].size(), 1U);
    EXPECT_EQ(first["requests"][0]["type"], "loss_eligibility_confirmation_required");

    const Response answered =
        server->postJson("/api/run/answers",
                         answerBody(1, "request-0001", R"("no_other_activity")"));

    ASSERT_EQ(answered.result_int(), 200U);
    ASSERT_EQ(jsonOf(answered)["runRevision"], 2);

    // The same files again: revision 3 with the answer of the first run.
    const Json restarted = jsonOf(server->upload(upload));

    EXPECT_EQ(restarted["runRevision"], 3);
    EXPECT_TRUE(restarted["requests"].empty());
    ASSERT_EQ(restarted["decisions"].size(), 1U);
    EXPECT_EQ(restarted["decisions"][0]["fromEarlierRun"], true);
    EXPECT_EQ(restarted["decisions"][0]["used"], true);

    // An answer given for revision 2, the one before the restart, is stale.
    const Response stale =
        server->postJson("/api/run/answers",
                         answerBody(2, "request-0001", R"("no_other_activity")"));

    ASSERT_EQ(stale.result_int(), 422U);
    EXPECT_EQ(jsonOf(stale)["rejections"][0]["messageKey"], "answer_rejected.stale_revision");
}

TEST_F(ServerRunTest, ARestartWithABrokenRateFileLeavesTheRunAsItWas) {
    const auto server = startServer();

    const Response first = server->upload(pipelineUpload(2024, false));

    ASSERT_EQ(first.result_int(), 200U);

    // A rate file without its header.
    const std::string files = std::string{kTwoBrokerFilesAndRates};
    const Response broken = server->upload(
        {multipartBody(requestJson(2024, files),
                       {{"file0", "export_a.csv", readTestData("pipeline", "export_2023_2024.csv")},
                        {"file1", "export_b.csv", readTestData("pipeline", "export_2024_2025.csv")},
                        {"file2", "bad_rates.csv", "this is not a rate file\n"}}),
         contentTypeHeader()});

    ASSERT_EQ(broken.result_int(), 422U);

    const Json fault = jsonOf(broken);

    ASSERT_TRUE(fault.contains("inputFault"));
    EXPECT_EQ(fault["inputFault"]["kind"], "ecb_rate_file");
    EXPECT_EQ(fault["inputFault"]["source"]["file"], "bad_rates.csv");
    EXPECT_TRUE(fault["inputFault"].contains("messageKey"));

    // The result before is unchanged.
    EXPECT_EQ(server->send("GET", "/api/run").body(), first.body());
}

TEST_F(ServerRunTest, ABrokenReferenceFileIsAnInputFault) {
    const auto server = startServer();
    const std::string files = R"([{"part":"file0","kind":"broker","broker":"trade_republic"},)"
                              R"({"part":"file1","kind":"reference"}])";

    // payers.csv without its header columns.
    const Response response = server->upload(
        {multipartBody(requestJson(2024, files),
                       {{"file0", "export_a.csv", readTestData("pipeline", "export_2023_2024.csv")},
                        {"file1", "payers.csv", "only_one_column\n"}}),
         contentTypeHeader()});

    ASSERT_EQ(response.result_int(), 422U);

    const Json fault = jsonOf(response);

    EXPECT_EQ(fault["inputFault"]["kind"], "reference_file");
    EXPECT_EQ(fault["inputFault"]["source"]["file"], "payers.csv");
}

TEST_F(ServerRunTest, DeletingAnswersRecalculatesAndChecksTheRevisionAndTheIds) {
    const auto server = startServer();

    ASSERT_EQ(server->upload(pipelineUpload(2024, false)).result_int(), 200U);
    ASSERT_EQ(
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("above_limit")"))
            .result_int(),
        200U);

    // Delete decision-0001: the deposit question is open again, revision 3.
    const Response deleted =
        server->postJson("/api/run/decisions/delete", deleteBody(2, R"(["decision-0001"])"));

    ASSERT_EQ(deleted.result_int(), 200U);

    const Json afterDelete = jsonOf(deleted);

    EXPECT_EQ(afterDelete["runRevision"], 3);
    EXPECT_TRUE(afterDelete["decisions"].empty());
    ASSERT_EQ(afterDelete["requests"].size(), 1U);
    EXPECT_EQ(afterDelete["requests"][0]["type"], "deposit_interest_total_required");

    // Answer again (revision 4), so there is a decision to delete.
    ASSERT_EQ(
        server->postJson("/api/run/answers", answerBody(3, "request-0001", R"("above_limit")"))
            .result_int(),
        200U);

    // An old revision.
    const Response stale =
        server->postJson("/api/run/decisions/delete", deleteBody(3, R"(["decision-0001"])"));

    ASSERT_EQ(stale.result_int(), 422U);
    EXPECT_EQ(
        jsonOf(stale),
        Json::parse(
            R"({"rejections":[{"index":0,"messageKey":"answer_rejected.stale_revision_delete",)"
            R"("messageParams":{"decisionId":"decision-0001"}}]})"));

    // An ID that is not listed.
    const Response unknown =
        server->postJson("/api/run/decisions/delete", deleteBody(4, R"(["decision-0099"])"));

    ASSERT_EQ(unknown.result_int(), 422U);
    EXPECT_EQ(
        jsonOf(unknown),
        Json::parse(R"({"rejections":[{"index":0,"messageKey":"answer_rejected.unknown_decision",)"
                    R"("messageParams":{"decisionId":"decision-0099"}}]})"));

    // Delete all.
    const Response all =
        server->postJson("/api/run/decisions/delete", R"({"runRevision":4,"all":true})");

    ASSERT_EQ(all.result_int(), 200U);
    EXPECT_TRUE(jsonOf(all)["decisions"].empty());
    EXPECT_EQ(jsonOf(all)["runRevision"], 5);
}

TEST_F(ServerRunTest, AnswerAndDeleteBeforeAnUploadAreNotFound) {
    const auto server = startServer();

    const Response answer =
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("above_limit")"));
    const Response deleted =
        server->postJson("/api/run/decisions/delete", R"({"runRevision":1,"all":true})");

    EXPECT_EQ(answer.result_int(), 404U);
    EXPECT_EQ(answer.body(), R"({"error":"no_run"})");
    EXPECT_EQ(deleted.result_int(), 404U);
    EXPECT_EQ(deleted.body(), R"({"error":"no_run"})");
}

TEST_F(ServerRunTest, ABodyThatIsNotJsonIsBadRequest) {
    const auto server = startServer();

    ASSERT_EQ(server->upload(pipelineUpload(2024, false)).result_int(), 200U);

    const Response answers = server->postJson("/api/run/answers", "not json");
    const Response deleted = server->postJson("/api/run/decisions/delete", "not json");

    EXPECT_EQ(answers.result_int(), 400U);
    EXPECT_EQ(answers.body(), R"({"error":"bad_request"})");
    EXPECT_EQ(deleted.result_int(), 400U);
    EXPECT_EQ(deleted.body(), R"({"error":"bad_request"})");
}

// ---- Source names, privacy and limits

TEST_F(ServerRunTest, TwoFilesWithOneNameAreTwoSources) {
    const auto server = startServer();
    const std::string files = std::string{kTwoBrokerFilesAndRates};

    const Json result = jsonOf(server->upload(
        {multipartBody(requestJson(2024, files),
                       {{"file0", "export.csv", readTestData("pipeline", "export_2023_2024.csv")},
                        {"file1", "export.csv", readTestData("pipeline", "export_2024_2025.csv")},
                        {"file2", "rates.csv", readTestData("cli", "report_ecb_rates.csv")}}),
         contentTypeHeader()}));

    // The payer warnings of the two coupons: p-coupon-1 is in the first file, p-coupon-2 in the
    // second. Both sources have the name export.csv and their own ID.
    std::set<std::string> sourceIds;

    for (const Json& diagnostic : result["diagnostics"])
    {
        for (const Json& source : diagnostic["sources"])
        {
            EXPECT_EQ(source["file"], "export.csv");
            sourceIds.insert(source["sourceId"]);
        }
    }

    EXPECT_EQ(sourceIds, (std::set<std::string>{"source-0001", "source-0002"}));
}

TEST_F(ServerRunTest, AFileNameWithAFolderKeepsOnlyTheName) {
    const auto server = startServer();
    const std::string files = std::string{kTwoBrokerFilesAndRates};

    const Response uploaded = server->upload(
        {multipartBody(requestJson(2024, files),
                       {{"file0",
                         "/home/someone/export.csv",
                         readTestData("pipeline", "export_2023_2024.csv")},
                        {"file1",
                         "C:\\Users\\someone\\export2.csv",
                         readTestData("pipeline", "export_2024_2025.csv")},
                        {"file2", "../../rates.csv", readTestData("cli", "report_ecb_rates.csv")}}),
         contentTypeHeader()});

    ASSERT_EQ(uploaded.result_int(), 200U);

    // The helper has already checked that no body holds a folder; the names are there.
    EXPECT_NE(uploaded.body().find("\"export.csv\""), std::string::npos);
    EXPECT_NE(uploaded.body().find("\"export2.csv\""), std::string::npos);
    EXPECT_NE(uploaded.body().find("\"rates.csv\""), std::string::npos);
}

TEST_F(ServerRunTest, AFileOverTheLimitIsRefusedAndOneOfExactlyTheLimitIsAccepted) {
    const auto server = startServer(1024);

    const Response tooLarge =
        server->upload({multipartBody(requestJson(2024, std::string{kOneBrokerFile}),
                                      {{"file0", "export.csv", std::string(1025, 'x')}}),
                        contentTypeHeader()});

    EXPECT_EQ(tooLarge.result_int(), 413U);
    EXPECT_EQ(tooLarge.body(), R"({"error":"file_too_large"})");

    // 1024 bytes of text are not a valid export, but the file is accepted: the run reports it.
    const Response exact =
        server->upload({multipartBody(requestJson(2024, std::string{kOneBrokerFile}),
                                      {{"file0", "export.csv", std::string(1024, 'x')}}),
                        contentTypeHeader()});

    ASSERT_EQ(exact.result_int(), 200U);
    EXPECT_EQ(jsonOf(exact)["status"], "failed");
}

TEST_F(ServerRunTest, ASuccessfulRunWritesNoFile) {
    const auto server = startServer();

    const auto listing = [this]() {
        std::set<std::string> names;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(mFolder))
        {
            names.insert(entry.path().string());
        }

        return names;
    };

    // The server gets only this test's state, runtime and launch folders.
    const std::set<std::string> before = listing();

    ASSERT_EQ(server->upload(pipelineUpload(2024, false)).result_int(), 200U);
    ASSERT_EQ(
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("above_limit")"))
            .result_int(),
        200U);
    ASSERT_EQ(server->send("GET", "/api/run/reports/capitalGains/xml?revision=2").result_int(),
              200U);

    // The uploads, the answer and the download left the same files as before.
    EXPECT_EQ(listing(), before);
}

TEST_F(ServerRunTest, APathKeyIsRefusedAndTheServerWritesNothing) {
    const auto server = startServer();

    const auto listing = [this]() {
        std::set<std::string> names;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(mFolder))
        {
            names.insert(entry.path().string());
        }

        return names;
    };

    const std::set<std::string> before = listing();

    const Response response = server->upload(
        {multipartBody(R"({"reportingYear":2024,"filingType":"original","taxpayer":)" +
                           std::string{kTaxpayerJson} +
                           R"(,"files":[{"part":"file0","kind":"broker","broker":"trade_republic",)"
                           R"("path":"/home/someone/export.csv"}]})",
                       {{"file0", "export.csv", "x"}}),
         contentTypeHeader()});

    EXPECT_EQ(response.result_int(), 400U);
    EXPECT_EQ(response.body(), R"({"error":"path_not_allowed"})");

    // The state, runtime and launch folders hold the same files as before.
    EXPECT_EQ(listing(), before);
}

TEST_F(ServerRunTest, AnUploadIsRefusedWithACodeForEveryInputProblem) {
    const auto server = startServer();

    // No broker file.
    const Response noBroker =
        server->upload({multipartBody(requestJson(2024, R"([{"part":"file0","kind":"ecb_rates"}])"),
                                      {{"file0", "rates.csv", "x"}}),
                        contentTypeHeader()});
    // A part that the request does not name.
    const Response unreferenced =
        server->upload({multipartBody(requestJson(2024, std::string{kOneBrokerFile}),
                                      {{"file0", "export.csv", "x"}, {"file1", "other.csv", "y"}}),
                        contentTypeHeader()});
    // A named part that is not uploaded.
    const Response missing = server->upload(
        {multipartBody(requestJson(2024, std::string{kOneBrokerFile}), {}), contentTypeHeader()});
    // A broker the application does not read.
    const Response unsupported = server->upload(
        {multipartBody(requestJson(2024, R"([{"part":"file0","kind":"broker","broker":"ibkr"}])"),
                       {{"file0", "export.csv", "x"}}),
         contentTypeHeader()});
    // Not a multipart body.
    const Response notMultipart = server->send("POST", "/api/run", "application/json", "{}");
    // No Content-Type header at all.
    const Response noContentType = server->send("POST", "/api/run", "", "{}");

    EXPECT_EQ(noBroker.body(), R"({"error":"no_broker_file"})");
    EXPECT_EQ(unreferenced.body(), R"({"error":"unreferenced_part"})");
    EXPECT_EQ(missing.body(), R"({"error":"missing_part"})");
    EXPECT_EQ(unsupported.body(), R"({"error":"unsupported_broker"})");
    EXPECT_EQ(notMultipart.body(), R"({"error":"bad_multipart"})");
    EXPECT_EQ(noContentType.body(), R"({"error":"bad_multipart"})");

    for (const Response* response :
         {&noBroker, &unreferenced, &missing, &unsupported, &notMultipart, &noContentType})
    {
        EXPECT_EQ(response->result_int(), 400U);
    }

    // None of them started a run.
    EXPECT_EQ(server->send("GET", "/api/run").result_int(), 404U);
}

TEST_F(ServerRunTest, DownloadChecksTheRevisionAndTheReportName) {
    const auto server = startServer();

    ASSERT_EQ(server->upload(pipelineUpload(2024, false)).result_int(), 200U);
    ASSERT_EQ(
        server->postJson("/api/run/answers", answerBody(1, "request-0001", R"("above_limit")"))
            .result_int(),
        200U);

    // Revision 1 is not current any more.
    const Response stale = server->send("GET", "/api/run/reports/capitalGains/xml?revision=1");

    EXPECT_EQ(stale.result_int(), 409U);
    EXPECT_EQ(stale.body(), R"({"error":"stale_revision"})");

    const Response withoutRevision = server->send("GET", "/api/run/reports/capitalGains/xml");

    EXPECT_EQ(withoutRevision.result_int(), 400U);
    EXPECT_EQ(withoutRevision.body(), R"({"error":"bad_request"})");

    EXPECT_EQ(server->send("GET", "/api/run/reports/unknown/xml?revision=2").result_int(), 404U);
}

TEST_F(ServerRunTest, EveryRunRouteNeedsTheSessionAndTheClientHeader) {
    const auto server = startServer();
    const std::vector<std::pair<std::string, std::string>> routes{
        {"POST", "/api/run"},
        {"GET", "/api/run"},
        {"POST", "/api/run/answers"},
        {"POST", "/api/run/decisions/delete"},
        {"GET", "/api/run/reports/capitalGains/xml?revision=1"},
    };

    for (const auto& [method, target] : routes)
    {
        // The body is small, so it is read before the 401.
        EXPECT_EQ(
            server->sendWith(method, target, "application/json", "{}", false, true).result_int(),
            401U)
            << target;
        EXPECT_EQ(
            server->sendWith(method, target, "application/json", "{}", true, false).result_int(),
            403U)
            << target;
    }
}

TEST_F(ServerRunTest, TheSecurityHeadersAreOnADownloadAnd413And422) {
    const auto expectSecurityHeaders = [](const Response& aResponse) {
        EXPECT_EQ(headerOf(aResponse, "X-Content-Type-Options"), "nosniff");
        EXPECT_EQ(headerOf(aResponse, "Cache-Control"), "no-store");
        EXPECT_EQ(headerOf(aResponse, "Cross-Origin-Resource-Policy"), "same-origin");
        EXPECT_FALSE(headerOf(aResponse, "Content-Security-Policy").empty());
        EXPECT_EQ(headerOf(aResponse, "Access-Control-Allow-Origin"), "");
    };

    // 200 with the XML, and 422 for a stale answer.
    const auto server = startServer();

    ASSERT_EQ(server->upload(pipelineUpload(2024, false)).result_int(), 200U);

    const Response download = server->send("GET", "/api/run/reports/capitalGains/xml?revision=1");
    const Response rejected =
        server->postJson("/api/run/answers", answerBody(0, "request-0001", R"("above_limit")"));

    ASSERT_EQ(download.result_int(), 200U);
    ASSERT_EQ(rejected.result_int(), 422U);
    expectSecurityHeaders(download);
    expectSecurityHeaders(rejected);

    // 413 from a server with a file limit of 1 KiB.
    const auto smallServer = startServer(1024, "small");
    const Response tooLarge =
        smallServer->upload({multipartBody(requestJson(2024, std::string{kOneBrokerFile}),
                                           {{"file0", "export.csv", std::string(2000, 'x')}}),
                             contentTypeHeader()});

    ASSERT_EQ(tooLarge.result_int(), 413U);
    expectSecurityHeaders(tooLarge);
}

} // namespace
