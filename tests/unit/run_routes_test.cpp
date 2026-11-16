#include "../support/log_capture.hpp"
#include "server/run_routes.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;

using Json = nlohmann::json;

// All rows are synthetic. The header is the one of the Trade Republic export.
constexpr std::string_view kHeaderOnly =
    "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,"
    "currency,original_amount,original_currency,fx_rate,description,transaction_id,"
    "counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

// 10 units bought at EUR 30 and sold at EUR 20 in 2025, with no purchase within 30 days of the
// sale: the run asks the 30-day loss question.
constexpr std::string_view kLossRows =
    "2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic "
    "Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,\n"
    "2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic "
    "Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,\n";

constexpr std::string_view kBrokerFileEntry =
    R"([{"part":"file0","kind":"broker","broker":"trade_republic"}])";

/// A committed synthetic file under `tests/test_data`, read byte for byte.
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

/// The request part and one file part of an upload, boundary `b`.
std::string uploadBody(int aYear,
                       std::string_view aFilesList,
                       std::string_view aFile0,
                       std::string_view aFile1 = {},
                       bool aDeveloperMode = false) {
    std::string body = "--b\r\nContent-Disposition: form-data; name=\"request\"\r\n\r\n"
                       R"({"reportingYear":)" +
                       std::to_string(aYear) + R"(,"filingType":"original","developerMode":)" +
                       (aDeveloperMode ? "true" : "false") +
                       R"(,"taxpayer":{"taxNumber":"12345679","isResident":true},"files":)" +
                       std::string{aFilesList} +
                       "}\r\n--b\r\nContent-Disposition: form-data; name=\"file0\"; "
                       "filename=\"synthetic.csv\"\r\n\r\n" +
                       std::string{aFile0} + "\r\n";

    if (!aFile1.empty())
    {
        body += "--b\r\nContent-Disposition: form-data; name=\"file1\"; "
                "filename=\"bad_rates.csv\"\r\n\r\n" +
                std::string{aFile1} + "\r\n";
    }

    return body + "--b--\r\n";
}

HttpRequest postUpload(std::string aBody) {
    return HttpRequest{.mMethod = "POST",
                       .mTarget = "/api/run",
                       .mHeaders = {{"Content-Type", "multipart/form-data; boundary=b"}},
                       .mBody = std::move(aBody)};
}

HttpRequest postJson(std::string aTarget, std::string aBody) {
    return HttpRequest{.mMethod = "POST",
                       .mTarget = std::move(aTarget),
                       .mHeaders = {{"Content-Type", "application/json"}},
                       .mBody = std::move(aBody)};
}

HttpResponse get(RunRoutes& aRoutes, const std::string& aTarget) {
    const std::size_t mark = aTarget.find('?');
    const std::string path = aTarget.substr(0, mark);
    const std::string query = mark == std::string::npos ? "" : aTarget.substr(mark + 1);

    return aRoutes.handle(HttpRequest{.mMethod = "GET", .mTarget = aTarget}, path, query);
}

HttpResponse post(RunRoutes& aRoutes, const HttpRequest& aRequest) {
    const std::string path = aRequest.mTarget.substr(0, aRequest.mTarget.find('?'));

    return aRoutes.handle(aRequest, path, "");
}

TEST(RunRoutesTest, UploadLimitIs33TimesTheFileLimit) {
    EXPECT_EQ(RunRoutes(1000).uploadBodyLimit(), 33000U);
}

TEST(RunRoutesTest, NoRunBeforeTheFirstUpload) {
    RunRoutes routes(100000);

    const HttpResponse result = get(routes, "/api/run");

    EXPECT_EQ(result.mStatus, 404U);
    EXPECT_EQ(result.mBody, R"({"error":"no_run"})");
    EXPECT_EQ(get(routes, "/api/run/reports/capitalGains/xml?revision=1").mStatus, 404U);
    EXPECT_EQ(post(routes, postJson("/api/run/answers", "{}")).mStatus, 404U);
    EXPECT_EQ(post(routes, postJson("/api/run/decisions/delete", "{}")).mStatus, 404U);
}

TEST(RunRoutesTest, UploadStartsARunAndTheResultIsTheSameAfterwards) {
    RunRoutes routes(100000);

    const HttpResponse started =
        post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, kHeaderOnly)));

    ASSERT_EQ(started.mStatus, 200U);

    const Json result = Json::parse(started.mBody);

    EXPECT_EQ(result["schemaVersion"], 2);
    EXPECT_EQ(result["runRevision"], 1);
    EXPECT_EQ(result["status"], "success");
    EXPECT_EQ(get(routes, "/api/run").mBody, started.mBody);
}

TEST(RunRoutesTest, SecondUploadRestartsTheRun) {
    RunRoutes routes(100000);

    ASSERT_EQ(post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, kHeaderOnly))).mStatus,
              200U);

    const HttpResponse restarted =
        post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, kHeaderOnly)));

    ASSERT_EQ(restarted.mStatus, 200U);
    EXPECT_EQ(Json::parse(restarted.mBody)["runRevision"], 2);
}

TEST(RunRoutesTest, UploadWithoutAContentTypeIsBadMultipart) {
    RunRoutes routes(100000);

    const HttpResponse response =
        post(routes, HttpRequest{.mMethod = "POST", .mTarget = "/api/run", .mBody = "{}"});

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"bad_multipart"})");
}

TEST(RunRoutesTest, UploadThatIsNotMultipartIsBadMultipart) {
    RunRoutes routes(100000);

    const HttpResponse response = post(routes, postJson("/api/run", "{}"));

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"bad_multipart"})");
}

TEST(RunRoutesTest, FileOverTheLimitIs413AndOneOfTheLimitIsAccepted) {
    RunRoutes routes(10);

    const HttpResponse tooLarge =
        post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, std::string(11, 'x'))));
    const HttpResponse exact =
        post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, std::string(10, 'x'))));

    EXPECT_EQ(tooLarge.mStatus, 413U);
    EXPECT_EQ(tooLarge.mBody, R"({"error":"file_too_large"})");
    EXPECT_EQ(exact.mStatus, 200U);
}

TEST(RunRoutesTest, FieldPartOverTheFileLimitIs413) {
    RunRoutes routes(10);

    // The part `file0` has no `filename`: it is a plain form field, 11 bytes under a 10-byte limit.
    const std::string tooLargeBody =
        "--b\r\nContent-Disposition: form-data; name=\"request\"\r\n\r\n"
        R"({"reportingYear":2024,"filingType":"original",)"
        R"("taxpayer":{"taxNumber":"12345679","isResident":true},)"
        R"("files":[{"part":"file0","kind":"broker","broker":"trade_republic"}]})"
        "\r\n--b\r\nContent-Disposition: form-data; name=\"file0\"\r\n\r\n"
        "xxxxxxxxxxx\r\n--b--\r\n";
    const std::string exactBody =
        "--b\r\nContent-Disposition: form-data; name=\"request\"\r\n\r\n"
        R"({"reportingYear":2024,"filingType":"original",)"
        R"("taxpayer":{"taxNumber":"12345679","isResident":true},)"
        R"("files":[{"part":"file0","kind":"broker","broker":"trade_republic"}]})"
        "\r\n--b\r\nContent-Disposition: form-data; name=\"file0\"\r\n\r\n"
        "xxxxxxxxxx\r\n--b--\r\n";

    const HttpResponse tooLarge = post(routes, postUpload(tooLargeBody));
    const HttpResponse exact = post(routes, postUpload(exactBody));

    EXPECT_EQ(tooLarge.mStatus, 413U);
    EXPECT_EQ(tooLarge.mBody, R"({"error":"file_too_large"})");
    EXPECT_EQ(exact.mStatus, 200U);
}

/// An upload whose `request` part is the given text and that has no file.
std::string uploadWithRequestPart(const std::string& aRequestPart) {
    return "--b\r\nContent-Disposition: form-data; name=\"request\"\r\n\r\n" + aRequestPart +
           "\r\n--b--\r\n";
}

TEST(RunRoutesTest, RequestPartOfExactlyTheBodyLimitIsNotRefusedForItsSize) {
    RunRoutes routes(100000);

    // 1048576 bytes of text are not JSON, so the request is refused as a bad request.
    const std::string requestPart(1048576, 'x');

    const HttpResponse response = post(routes, postUpload(uploadWithRequestPart(requestPart)));

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"bad_request"})");
}

TEST(RunRoutesTest, RequestPartOneByteOverTheBodyLimitIs413) {
    RunRoutes routes(100000);

    // 1048577 bytes: one more than the limit of the request part.
    const std::string requestPart(1048577, 'x');

    const HttpResponse response = post(routes, postUpload(uploadWithRequestPart(requestPart)));

    EXPECT_EQ(response.mStatus, 413U);
    EXPECT_EQ(response.mBody, R"({"error":"request_too_large"})");
}

TEST(RunRoutesTest, RequestThatIsNotUsableGivesItsCode) {
    RunRoutes routes(100000);

    // The list names no broker file.
    const HttpResponse response =
        post(routes, postUpload(uploadBody(2024, R"([{"part":"file0","kind":"ecb_rates"}])", "x")));

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"no_broker_file"})");
}

TEST(RunRoutesTest, BrokenRateFileIsAnInputFaultAndKeepsTheRun) {
    RunRoutes routes(100000);
    const std::string files =
        R"([{"part":"file0","kind":"broker","broker":"trade_republic"},{"part":"file1","kind":"ecb_rates"}])";

    // At the start there is no run, so the fault comes from `start`.
    const HttpResponse atStart =
        post(routes, postUpload(uploadBody(2024, files, kHeaderOnly, "this is not a rate file\n")));

    ASSERT_EQ(atStart.mStatus, 422U);
    EXPECT_EQ(Json::parse(atStart.mBody)["inputFault"]["kind"], "ecb_rate_file");
    EXPECT_EQ(get(routes, "/api/run").mStatus, 404U);

    // With a run, the fault comes from `restart` and the run stays.
    const HttpResponse first =
        post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, kHeaderOnly)));
    const HttpResponse atRestart =
        post(routes, postUpload(uploadBody(2024, files, kHeaderOnly, "this is not a rate file\n")));

    ASSERT_EQ(atRestart.mStatus, 422U);
    EXPECT_EQ(Json::parse(atRestart.mBody)["inputFault"]["source"]["file"], "bad_rates.csv");
    EXPECT_EQ(get(routes, "/api/run").mBody, first.mBody);
}

TEST(RunRoutesTest, AnswerRecalculatesAndTheXmlCanBeDownloaded) {
    RunRoutes routes(100000);
    const std::string file = std::string{kHeaderOnly} + std::string{kLossRows};

    const Json started =
        Json::parse(post(routes, postUpload(uploadBody(2025, kBrokerFileEntry, file))).mBody);

    ASSERT_EQ(started["requests"].size(), 1U);
    EXPECT_EQ(started["requests"][0]["type"], "loss_eligibility_confirmation_required");

    // No XML while the question is open.
    EXPECT_EQ(get(routes, "/api/run/reports/capitalGains/xml?revision=1").mStatus, 404U);

    const HttpResponse answered = post(
        routes,
        postJson(
            "/api/run/answers",
            R"({"runRevision":1,"answers":[{"requestId":"request-0001","answer":"no_other_activity"}]})"));

    ASSERT_EQ(answered.mStatus, 200U);
    EXPECT_EQ(Json::parse(answered.mBody)["runRevision"], 2);

    const HttpResponse xml = get(routes, "/api/run/reports/capitalGains/xml?revision=2");

    ASSERT_EQ(xml.mStatus, 200U);
    EXPECT_NE(xml.mBody.find("<Envelope"), std::string::npos);

    const auto disposition =
        std::find_if(xml.mHeaders.begin(), xml.mHeaders.end(), [](const HttpField& aField) {
            return aField.mName == "Content-Disposition";
        });

    ASSERT_NE(disposition, xml.mHeaders.end());
    EXPECT_EQ(disposition->mValue, "attachment; filename=\"Doh-KDVP_2025.xml\"");
}

TEST(RunRoutesTest, DownloadChecksTheReportAndTheRevision) {
    RunRoutes routes(100000);

    ASSERT_EQ(post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, kHeaderOnly))).mStatus,
              200U);

    EXPECT_EQ(get(routes, "/api/run/reports/unknown/xml?revision=1").mStatus, 404U);
    EXPECT_EQ(get(routes, "/api/run/reports/capitalGains/xml").mStatus, 400U);
    EXPECT_EQ(get(routes, "/api/run/reports/capitalGains/xml?revision=").mStatus, 400U);
    EXPECT_EQ(get(routes, "/api/run/reports/capitalGains/xml?revision=1x").mStatus, 400U);
    EXPECT_EQ(get(routes, "/api/run/reports/capitalGains/xml?revision=2").mStatus, 409U);

    // The run has no data, so there is no file for any form.
    const HttpResponse noXml = get(routes, "/api/run/reports/capitalGains/xml?revision=1");

    EXPECT_EQ(noXml.mStatus, 404U);
    EXPECT_EQ(noXml.mBody, R"({"error":"no_xml"})");
}

TEST(RunRoutesTest, AnswerThatDoesNotFitOrIsStaleIsRejected) {
    RunRoutes routes(100000);
    const std::string file = std::string{kHeaderOnly} + std::string{kLossRows};

    ASSERT_EQ(post(routes, postUpload(uploadBody(2025, kBrokerFileEntry, file))).mStatus, 200U);

    // "skip" is not an answer to the loss question.
    const HttpResponse notFitting = post(
        routes,
        postJson("/api/run/answers",
                 R"({"runRevision":1,"answers":[{"requestId":"request-0001","answer":"skip"}]})"));

    ASSERT_EQ(notFitting.mStatus, 422U);
    EXPECT_EQ(Json::parse(notFitting.mBody)["rejections"][0]["messageKey"],
              "answer_rejected.answer_does_not_fit");

    // Revision 0 is stale.
    const HttpResponse stale = post(
        routes,
        postJson(
            "/api/run/answers",
            R"({"runRevision":0,"answers":[{"requestId":"request-0001","answer":"no_other_activity"}]})"));

    ASSERT_EQ(stale.mStatus, 422U);
    EXPECT_EQ(Json::parse(stale.mBody)["rejections"][0]["messageKey"],
              "answer_rejected.stale_revision");
    EXPECT_EQ(Json::parse(get(routes, "/api/run").mBody)["runRevision"], 1);

    EXPECT_EQ(post(routes, postJson("/api/run/answers", "not json")).mStatus, 400U);
}

TEST(RunRoutesTest, DeleteOfARecordedAnswerAsksTheQuestionAgain) {
    RunRoutes routes(100000);
    const std::string file = std::string{kHeaderOnly} + std::string{kLossRows};

    ASSERT_EQ(post(routes, postUpload(uploadBody(2025, kBrokerFileEntry, file))).mStatus, 200U);
    ASSERT_EQ(
        post(
            routes,
            postJson(
                "/api/run/answers",
                R"({"runRevision":1,"answers":[{"requestId":"request-0001","answer":"no_other_activity"}]})"))
            .mStatus,
        200U);

    const HttpResponse unknown =
        post(routes,
             postJson("/api/run/decisions/delete",
                      R"({"runRevision":2,"decisionIds":["decision-0099"]})"));

    ASSERT_EQ(unknown.mStatus, 422U);
    EXPECT_EQ(Json::parse(unknown.mBody)["rejections"][0]["messageKey"],
              "answer_rejected.unknown_decision");

    const HttpResponse deleted =
        post(routes,
             postJson("/api/run/decisions/delete",
                      R"({"runRevision":2,"decisionIds":["decision-0001"]})"));

    ASSERT_EQ(deleted.mStatus, 200U);

    const Json result = Json::parse(deleted.mBody);

    EXPECT_EQ(result["runRevision"], 3);
    EXPECT_TRUE(result["decisions"].empty());
    EXPECT_EQ(result["requests"].size(), 1U);

    EXPECT_EQ(post(routes, postJson("/api/run/decisions/delete", "not json")).mStatus, 400U);
}

TEST(RunRoutesTest, UnknownPathBelowRunIsNotFoundAndOtherMethodsAreNotAllowed) {
    RunRoutes routes(100000);

    EXPECT_EQ(get(routes, "/api/run/nothing").mStatus, 404U);

    // No report name between `/api/run/reports/` and `/xml`: an unknown path (empty body), not
    // the download (which would say `no_run`).
    EXPECT_EQ(get(routes, "/api/run/reports/xml").mStatus, 404U);
    EXPECT_EQ(get(routes, "/api/run/reports/xml").mBody, "");
    EXPECT_EQ(get(routes, "/api/run/reports//xml").mStatus, 404U);
    EXPECT_EQ(get(routes, "/api/run/reports//xml").mBody, "");

    EXPECT_EQ(get(routes, "/api/run/answers").mStatus, 405U);
    EXPECT_EQ(get(routes, "/api/run/decisions/delete").mStatus, 405U);
    EXPECT_EQ(routes.handle(HttpRequest{.mMethod = "DELETE", .mTarget = "/api/run"}, "/api/run", "")
                  .mStatus,
              405U);
    EXPECT_EQ(post(routes, postJson("/api/run/reports/capitalGains/xml", "{}")).mStatus, 405U);
}

TEST(RunRoutesTest, MoreThan32FilesIs400TooManyFilesAndLogsOneWarningWithTheLimit) {
    RunRoutes routes(100000);
    LogCapture log;

    std::string list = "[";

    for (int index = 0; index < 33; ++index)
    {
        list += std::string(index == 0 ? "" : ",") + R"({"part":"f)" + std::to_string(index) +
                R"(","kind":"broker","broker":"trade_republic"})";
    }

    list += "]";

    const HttpResponse response = post(routes, postUpload(uploadBody(2024, list, "x")));

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"too_many_files"})");
    EXPECT_EQ(log.count("Run request refused"), 1U);
    EXPECT_EQ(log.count("Run request refused: 400 too_many_files (limit 32)"), 1U);
}

TEST(RunRoutesTest, ThirtyThreeFilePartsAre400TooManyFilesAndLogOneWarningWithTheLimit) {
    RunRoutes routes(100000);
    LogCapture log;

    // The request names 33 files f0..f32 and the upload has 33 file parts: 34 parts in all.
    std::string list = "[";
    std::string fileParts;

    for (int index = 0; index < 33; ++index)
    {
        const std::string name = "f" + std::to_string(index);

        list += std::string(index == 0 ? "" : ",") + R"({"part":")" + name +
                R"(","kind":"broker","broker":"trade_republic"})";
        fileParts += "--b\r\nContent-Disposition: form-data; name=\"" + name +
                     "\"; filename=\"a.csv\"\r\n\r\nx\r\n";
    }

    list += "]";

    const std::string body = "--b\r\nContent-Disposition: form-data; name=\"request\"\r\n\r\n"
                             R"({"reportingYear":2024,"filingType":"original",)"
                             R"("taxpayer":{"taxNumber":"12345679","isResident":true},"files":)" +
                             list + "}\r\n" + fileParts + "--b--\r\n";

    const HttpResponse response = post(routes, postUpload(body));

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"too_many_files"})");
    EXPECT_EQ(log.count("Run request refused"), 1U);
    EXPECT_EQ(log.count("Run request refused: 400 too_many_files (limit 32)"), 1U);
}

TEST(RunRoutesTest, BadMultipartLogsAFixedReasonWord) {
    RunRoutes routes(100000);
    LogCapture log;

    // No Content-Type header.
    const HttpResponse noType =
        post(routes,
             HttpRequest{.mMethod = "POST", .mTarget = "/api/run", .mBody = "--b\r\n--b--\r\n"});

    // The part has no `name`.
    const HttpResponse noName =
        post(routes, postUpload("--b\r\nContent-Disposition: form-data\r\n\r\nx\r\n--b--\r\n"));

    // The body has no closing line.
    const HttpResponse noClosing =
        post(routes,
             postUpload("--b\r\nContent-Disposition: form-data; name=\"request\"\r\n\r\nx\r\n"));

    EXPECT_EQ(noType.mBody, R"({"error":"bad_multipart"})");
    EXPECT_EQ(noName.mBody, R"({"error":"bad_multipart"})");
    EXPECT_EQ(noClosing.mBody, R"({"error":"bad_multipart"})");

    EXPECT_EQ(log.count("Run request refused: 400 bad_multipart (content_type)"), 1U);
    EXPECT_EQ(log.count("Run request refused: 400 bad_multipart (part_header)"), 1U);
    EXPECT_EQ(log.count("Run request refused: 400 bad_multipart (body)"), 1U);
    EXPECT_EQ(log.count("Run request refused"), 3U);
}

TEST(RunRoutesTest, InputFaultLogsItsKindAndMessageKeyButNoFileName) {
    RunRoutes routes(100000);
    LogCapture log;
    const std::string rateFiles =
        R"([{"part":"file0","kind":"broker","broker":"trade_republic"},{"part":"file1","kind":"ecb_rates"}])";
    const std::string referenceFiles =
        R"([{"part":"file0","kind":"broker","broker":"trade_republic"},{"part":"file1","kind":"reference"}])";

    ASSERT_EQ(
        post(routes,
             postUpload(uploadBody(2024, rateFiles, kHeaderOnly, "this is not a rate file\n")))
            .mStatus,
        422U);
    ASSERT_EQ(
        post(routes, postUpload(uploadBody(2024, referenceFiles, kHeaderOnly, "only_one_column\n")))
            .mStatus,
        422U);

    EXPECT_EQ(
        log.count(
            "Run request refused: 422 input_fault (rate_file invalid_header.rate_file_columns)"),
        1U);
    EXPECT_EQ(
        log.count(
            "Run request refused: 422 input_fault (reference_file reference_file.unknown_name)"),
        1U);
    EXPECT_EQ(log.count("Run request refused"), 2U);
    EXPECT_EQ(log.text().find("bad_rates.csv"), std::string::npos);
}

TEST(RunRoutesTest, RejectedAnswersLogTheCountAndTheMessageKeysOnly) {
    RunRoutes routes(100000);
    const std::string file = std::string{kHeaderOnly} + std::string{kLossRows};

    ASSERT_EQ(post(routes, postUpload(uploadBody(2025, kBrokerFileEntry, file))).mStatus, 200U);

    LogCapture log;

    // Revision 0 is stale.
    ASSERT_EQ(
        post(
            routes,
            postJson(
                "/api/run/answers",
                R"({"runRevision":0,"answers":[{"requestId":"request-0001","answer":"no_other_activity"}]})"))
            .mStatus,
        422U);

    EXPECT_EQ(
        log.count(
            "Run request refused: 422 answer_rejected (1: answer_rejected.stale_revision x1)"),
        1U);
    EXPECT_EQ(log.text().find("request-0001"), std::string::npos);
}

TEST(RunRoutesTest, FileOverTheLimitLogsOneWarningWithTheLimitAndNoFileData) {
    RunRoutes routes(10);
    LogCapture log;

    const HttpResponse response =
        post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, "SECRETCONTENT")));

    EXPECT_EQ(response.mStatus, 413U);
    EXPECT_EQ(log.count("Run request refused"), 1U);
    EXPECT_EQ(log.count("Run request refused: 413 file_too_large (limit 10)"), 1U);

    // Neither the file name of the part nor its content is in the log.
    EXPECT_EQ(log.text().find("synthetic.csv"), std::string::npos);
    EXPECT_EQ(log.text().find("SECRETCONTENT"), std::string::npos);
}

TEST(RunRoutesTest, OtherRefusalsLogTheirStatusAndCode) {
    RunRoutes routes(100000);
    LogCapture log;

    EXPECT_EQ(get(routes, "/api/run").mStatus, 404U);
    EXPECT_EQ(post(routes, postUpload(uploadWithRequestPart("not json"))).mStatus, 400U);
    EXPECT_EQ(get(routes, "/api/run/nothing").mStatus, 404U);
    EXPECT_EQ(get(routes, "/api/run/answers").mStatus, 405U);

    EXPECT_EQ(log.count("Run request refused: 404 no_run"), 1U);
    EXPECT_EQ(log.count("Run request refused: 400 bad_request"), 1U);
    EXPECT_EQ(log.count("Run request refused: 404 not_found"), 1U);
    EXPECT_EQ(log.count("Run request refused: 405 method_not_allowed"), 1U);
    EXPECT_EQ(log.count("Run request refused"), 4U);
}

/// An upload whose taxpayer object is the given text.
std::string uploadWithTaxpayer(std::string_view aTaxpayer) {
    return uploadWithRequestPart(R"({"reportingYear":2024,"filingType":"original","taxpayer":)" +
                                 std::string{aTaxpayer} + R"(,"files":[]})");
}

TEST(RunRoutesTest, BadTaxpayerNamesTheFieldInTheBodyAndTheLog) {
    RunRoutes routes(100000);
    LogCapture log;

    const HttpResponse response =
        post(routes,
             postUpload(uploadWithTaxpayer(R"({"taxNumber":"1234567","isResident":true})")));

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"bad_taxpayer","field":"taxNumber"})");
    EXPECT_EQ(log.count("Run request refused"), 1U);
    EXPECT_EQ(log.count("Run request refused: 400 bad_taxpayer (taxNumber)"), 1U);

    // The value the user typed is not in the log.
    EXPECT_EQ(log.text().find("1234567"), std::string::npos);
}

TEST(RunRoutesTest, TaxpayerWithAnUnknownKeyHasNoFieldAndTheKeyIsNotLogged) {
    RunRoutes routes(100000);
    LogCapture log;

    const HttpResponse response =
        post(routes,
             postUpload(uploadWithTaxpayer(
                 R"({"taxNumber":"12345679","isResident":true,"shoeSize":"42"})")));

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"bad_taxpayer"})");
    EXPECT_EQ(log.count("Run request refused: 400 bad_taxpayer"), 1U);
    EXPECT_EQ(log.text().find("shoeSize"), std::string::npos);
}

TEST(RunRoutesTest, TaxpayerThatIsNotAnObjectStaysBadRequest) {
    RunRoutes routes(100000);

    const HttpResponse response = post(routes, postUpload(uploadWithTaxpayer("[]")));

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"bad_request"})");
}

TEST(RunRoutesTest, AcceptedUploadWritesNoRefusalLine) {
    RunRoutes routes(100000);
    LogCapture log;

    ASSERT_EQ(post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, kHeaderOnly))).mStatus,
              200U);

    EXPECT_EQ(log.count("refused"), 0U);
}

TEST(RunRoutesTest, AStartedRunLogsItsRequestAndItsResultWithoutValues) {
    RunRoutes routes(100000);
    LogCapture log;

    ASSERT_EQ(post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, kHeaderOnly))).mStatus,
              200U);

    EXPECT_EQ(log.count("Run started: year 2024, filing original, developer mode off, brokers "
                        "[Trade Republic x1], ecb rate files 0, reference files 0"),
              1U);
    EXPECT_EQ(log.count("Run result: revision=1 status=success capitalGains=no_data "
                        "dividends=no_data bankInterest=no_data otherInterest=no_data requests=0 "
                        "errors=0 warnings=0"),
              1U);
    EXPECT_EQ(log.count("Run details: requests [none]; errors [none]; warnings [none]"), 1U);

    // No file name and no taxpayer value.
    EXPECT_EQ(log.text().find("synthetic.csv"), std::string::npos);
    EXPECT_EQ(log.text().find("12345679"), std::string::npos);
}

TEST(RunRoutesTest, ASecondUploadLogsARestartWithTheNewRevision) {
    RunRoutes routes(100000);

    ASSERT_EQ(post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, kHeaderOnly))).mStatus,
              200U);

    LogCapture log;

    ASSERT_EQ(post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, kHeaderOnly))).mStatus,
              200U);

    EXPECT_EQ(log.count("Run restarted: year 2024, filing original"), 1U);
    EXPECT_EQ(log.count("Run result: revision=2 status=success"), 1U);
    EXPECT_EQ(log.count("Run started"), 0U);
}

TEST(RunRoutesTest, AnswersAndDownloadsLogTheirStepWithTheOpenQuestionTypes) {
    RunRoutes routes(100000);
    const std::string file = std::string{kHeaderOnly} + std::string{kLossRows};
    LogCapture log;

    ASSERT_EQ(post(routes, postUpload(uploadBody(2025, kBrokerFileEntry, file))).mStatus, 200U);

    // The open loss question is named by its type, without the ISIN or the name.
    EXPECT_EQ(log.count("Run result: revision=1 status=needs_input capitalGains=needs_input"), 1U);
    EXPECT_EQ(log.count("Run details: requests [loss_eligibility_confirmation_required x1]; "), 1U);

    ASSERT_EQ(
        post(
            routes,
            postJson(
                "/api/run/answers",
                R"({"runRevision":1,"answers":[{"requestId":"request-0001","answer":"no_other_activity"}]})"))
            .mStatus,
        200U);

    EXPECT_EQ(log.count("Answers accepted: 1 [loss_eligibility_confirmation_required x1]"), 1U);
    EXPECT_EQ(log.count("Run result: revision=2 status=success capitalGains=generated"), 1U);

    const HttpResponse xml = get(routes, "/api/run/reports/capitalGains/xml?revision=2");

    ASSERT_EQ(xml.mStatus, 200U);

    EXPECT_EQ(log.count("Report downloaded: capitalGains, revision 2, " +
                        std::to_string(xml.mBody.size()) + " bytes"),
              1U);
    EXPECT_EQ(log.count("unsafe"), 0U);

    ASSERT_EQ(post(routes,
                   postJson("/api/run/decisions/delete",
                            R"({"runRevision":2,"decisionIds":["decision-0001"]})"))
                  .mStatus,
              200U);

    EXPECT_EQ(log.count("Answers deleted: 1"), 1U);
    EXPECT_EQ(log.count("Run result: revision=3 status=needs_input"), 1U);

    ASSERT_EQ(post(routes, postJson("/api/run/decisions/delete", R"({"runRevision":3,"all":true})"))
                  .mStatus,
              200U);

    EXPECT_EQ(log.count("Answers deleted: all"), 1U);

    // No ISIN, name, amount or request ID in any line.
    EXPECT_EQ(log.text().find("XX0000000001"), std::string::npos);
    EXPECT_EQ(log.text().find("Synthetic"), std::string::npos);
    EXPECT_EQ(log.text().find("request-0001"), std::string::npos);
}

TEST(RunRoutesTest, RejectedAnswersOfOneKeyAreCountedOnceInTheLogLine) {
    RunRoutes routes(100000);
    const std::string file = std::string{kHeaderOnly} + std::string{kLossRows};

    ASSERT_EQ(post(routes, postUpload(uploadBody(2025, kBrokerFileEntry, file))).mStatus, 200U);

    LogCapture log;

    // Three answers for requests that do not exist (only request-0001 is open).
    const HttpResponse rejected = post(
        routes,
        postJson(
            "/api/run/answers",
            R"({"runRevision":1,"answers":[{"requestId":"request-0091","answer":"no_other_activity"},)"
            R"({"requestId":"request-0092","answer":"no_other_activity"},)"
            R"({"requestId":"request-0093","answer":"no_other_activity"}]})"));

    ASSERT_EQ(rejected.mStatus, 422U);

    // One line: the count 3 and the key once with x3, not the key three times.
    EXPECT_EQ(log.count("Run request refused: 422 answer_rejected "
                        "(3: answer_rejected.unknown_request x3)"),
              1U);
    EXPECT_EQ(log.text().find("request-009"), std::string::npos);
}

TEST(RunRoutesTest, AResultRequestLogsItsRevisionAsDebugOnly) {
    RunRoutes routes(100000);

    ASSERT_EQ(post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, kHeaderOnly))).mStatus,
              200U);

    LogCapture quiet;

    ASSERT_EQ(get(routes, "/api/run").mStatus, 200U);

    EXPECT_EQ(quiet.count("Run result requested"), 0U);

    LogCapture verbose;
    LogCapture::showDebug();

    ASSERT_EQ(get(routes, "/api/run").mStatus, 200U);

    EXPECT_EQ(verbose.count("Run result requested: revision 1"), 1U);
}

TEST(RunRoutesTest, ADeveloperExportDownloadIsLoggedAsUnsafe) {
    // XX0000000001: the 2024 sale of 0.5 units is 0.15 units short, so the history is incomplete.
    RunRoutes routes(100000);
    const std::string file = readTestData("cli", "developer_export.csv");

    const Json started = Json::parse(
        post(routes, postUpload(uploadBody(2024, kBrokerFileEntry, file, {}, true))).mBody);

    ASSERT_EQ(started["requests"].size(), 1U);
    EXPECT_EQ(started["requests"][0]["type"], "capital_gains_failure_resolution_required");

    // The choice, then the three confirmations; each answer is a new revision.
    Json current = Json::parse(
        post(
            routes,
            postJson(
                "/api/run/answers",
                R"({"runRevision":1,"answers":[{"requestId":"request-0001","answer":"developer_export"}]})"))
            .mBody);

    for (std::uint64_t step = 1; step <= 3; ++step)
    {
        ASSERT_EQ(current["requests"].size(), 1U) << step;

        const std::string body =
            R"({"runRevision":)" + std::to_string(step + 1) + R"(,"answers":[{"requestId":")" +
            current["requests"][0]["id"].get<std::string>() + R"(","answer":"confirm"}]})";

        current = Json::parse(post(routes, postJson("/api/run/answers", body)).mBody);
    }

    ASSERT_EQ(current["runRevision"], 5);

    LogCapture log;
    const HttpResponse xml = get(routes, "/api/run/reports/capitalGains/xml?revision=5");

    ASSERT_EQ(xml.mStatus, 200U);

    EXPECT_EQ(log.count("Report downloaded: capitalGains, revision 5, " +
                        std::to_string(xml.mBody.size()) + " bytes, unsafe developer export"),
              1U);
}

TEST(RunRoutesTest, AFailedRouteLogsItsWordAndTheExceptionKindButNotTheText) {
    LogCapture log;

    try
    { throw std::runtime_error("secret detail of /home/someone"); } catch (...)
    { logRouteFailure("upload"); }

    EXPECT_EQ(log.count("Run request failed: upload (std::runtime_error)"), 1U);
    EXPECT_EQ(log.text().find("secret"), std::string::npos);
}

} // namespace
