#pragma once

// The helpers of the integration tests that run the whole application through a real server: the
// synthetic fixtures and uploads, a server with a logged-in session and the answer calls. They are
// shared by ServerRunTest (Trade Republic runs and the API rules) and the mixed-broker run tests.

#include "server_test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace taxbroker::servertest {

using Json = nlohmann::json;

// ---- Fixtures

/// A committed file under `tests/test_data`, read byte for byte.
inline std::string readTestData(std::string_view aFolder, std::string_view aName) {
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
inline constexpr std::string_view kTradeRepublicHeader =
    "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,"
    "currency,original_amount,original_currency,fx_rate,description,transaction_id,"
    "counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

/// A Trade Republic file made of the header and the given rows.
inline std::string csvOfRows(const std::vector<std::string_view>& aRows) {
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
inline constexpr std::string_view kLossBuy =
    R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)";
inline constexpr std::string_view kLossSell =
    R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)";

/// The taxpayer of the golden files.
inline constexpr std::string_view kTaxpayerJson = R"({
    "taxNumber": "12345679",
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

inline constexpr std::string_view kBoundary = "tbrBoundary7MA4YWxkTrZu0gW";

/// The `multipart/form-data` body: the part `request` first, then the files.
inline std::string multipartBody(const std::string& aRequestJson,
                                 const std::vector<UploadFile>& aFiles) {
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
inline std::string
requestJson(int aYear, const std::string& aFilesList, bool aDeveloperMode = false) {
    return R"({"reportingYear":)" + std::to_string(aYear) +
           R"(,"filingType":"original","developerMode":)" + (aDeveloperMode ? "true" : "false") +
           R"(,"taxpayer":)" + std::string{kTaxpayerJson} + R"(,"files":)" + aFilesList + "}";
}

/// The `files` list for one broker file, `file0`.
inline constexpr std::string_view kOneBrokerFile =
    R"([{"part":"file0","kind":"broker","broker":"trade_republic"}])";

/// The `files` list for two broker files and the rate file.
inline constexpr std::string_view kTwoBrokerFilesAndRates =
    R"([{"part":"file0","kind":"broker","broker":"trade_republic"},)"
    R"({"part":"file1","kind":"broker","broker":"trade_republic"},)"
    R"({"part":"file2","kind":"ecb_rates"}])";

/// The pipeline upload: both exports and the made-up rate file.
struct PipelineUpload {
    std::string mBody;
    std::string mContentType;
};

inline std::string contentTypeHeader() {
    return "multipart/form-data; boundary=" + std::string{kBoundary};
}

inline PipelineUpload pipelineUpload(int aYear, bool aReversed) {
    const std::string first = readTestData("pipeline", "export_2023_2024.csv");
    const std::string second = readTestData("pipeline", "export_2024_2025.csv");

    return {multipartBody(requestJson(aYear, std::string{kTwoBrokerFilesAndRates}),
                          {{"file0", "export_a.csv", aReversed ? second : first},
                           {"file1", "export_b.csv", aReversed ? first : second},
                           {"file2", "rates.csv", readTestData("cli", "report_ecb_rates.csv")}}),
            contentTypeHeader()};
}

/// An upload of one Trade Republic file made of the given rows.
inline PipelineUpload oneFileUpload(int aYear, const std::vector<std::string_view>& aRows) {
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
                     {"XDG_DATA_HOME", (aFolder / "data").string()},
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
inline Json jsonOf(const Response& aResponse) {
    return Json::parse(aResponse.body());
}

/// The text of a header of the response.
inline std::string headerOf(const Response& aResponse, std::string_view aName) {
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
inline std::string
answerBody(std::uint64_t aRevision, const std::string& aRequestId, const std::string& aAnswerJson) {
    return R"({"runRevision":)" + std::to_string(aRevision) + R"(,"answers":[{"requestId":")" +
           aRequestId + R"(","answer":)" + aAnswerJson + "}]}";
}

/// The delete call body for the given decision IDs.
inline std::string deleteBody(std::uint64_t aRevision, const std::string& aIdsList) {
    return R"({"runRevision":)" + std::to_string(aRevision) + R"(,"decisionIds":)" + aIdsList + "}";
}

} // namespace taxbroker::servertest
