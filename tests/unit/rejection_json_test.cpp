#include "taxbroker/api/answer_json.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {
using namespace taxbroker;
using taxbroker::api::serializeInputFaultJson;
using taxbroker::api::serializeRejectionsJson;

/// The JSON value of a text: the order of the keys inside an object is not part of the contract.
nlohmann::json parsed(const std::string& aText) {
    return nlohmann::json::parse(aText);
}

/// A rejection as the session makes it, without the English sentence (the JSON does not use it).
RejectedAnswer rejection(std::size_t aIndex,
                         AnswerRejection aReason,
                         MessageKey aKey,
                         std::vector<MessageParam> aParams) {
    return RejectedAnswer{.mIndex = aIndex,
                          .mReason = aReason,
                          .mMessage = "An English sentence.",
                          .mMessageKey = aKey,
                          .mMessageParams = std::move(aParams)};
}

TEST(RejectionJsonTest, StaleRevisionAtIndexZero) {
    const std::string json =
        serializeRejectionsJson({rejection(0,
                                           AnswerRejection::StaleRevision,
                                           MessageKey::AnswerRejectedStaleRevision,
                                           {textParam("requestId", "request-0001")})});

    EXPECT_EQ(json,
              R"({"rejections":[{"index":0,"messageKey":"answer_rejected.stale_revision",)"
              R"("messageParams":{"requestId":"request-0001"}}]})");
}

TEST(RejectionJsonTest, RatioNotPositiveNamesTheField) {
    const std::string json = serializeRejectionsJson(
        {rejection(2,
                   AnswerRejection::RatioNotPositive,
                   MessageKey::AnswerRejectedRatioNotPositive,
                   {textParam("requestId", "request-0003"), textParam("field", "newShares")})});

    EXPECT_EQ(
        parsed(json),
        parsed(R"({"rejections":[{"index":2,"messageKey":"answer_rejected.ratio_not_positive",)"
               R"("messageParams":{"requestId":"request-0003","field":"newShares"}}]})"));
}

TEST(RejectionJsonTest, EffectiveDateOutOfRangeNamesTheTwoDates) {
    const std::string json =
        serializeRejectionsJson({rejection(0,
                                           AnswerRejection::EffectiveDateOutOfRange,
                                           MessageKey::AnswerRejectedEffectiveDateOutOfRange,
                                           {textParam("requestId", "request-0001"),
                                            textParam("earliestDate", "2024-05-01"),
                                            textParam("latestDate", "2024-06-30")})});

    EXPECT_EQ(
        parsed(json),
        parsed(
            R"({"rejections":[{"index":0,"messageKey":"answer_rejected.effective_date_out_of_range",)"
            R"("messageParams":{"requestId":"request-0001","earliestDate":"2024-05-01",)"
            R"("latestDate":"2024-06-30"}}]})"));
}

TEST(RejectionJsonTest, EveryRejectionWithARequestIdHasItsKey) {
    struct Row {
        AnswerRejection mReason{};
        MessageKey mKey{};
        std::string mExpectedKey;
    };

    const std::vector<Row> rows{
        {AnswerRejection::StaleRevision,
         MessageKey::AnswerRejectedStaleRevision,
         "answer_rejected.stale_revision"},
        {AnswerRejection::UnknownRequest,
         MessageKey::AnswerRejectedUnknownRequest,
         "answer_rejected.unknown_request"},
        {AnswerRejection::DuplicateAnswer,
         MessageKey::AnswerRejectedDuplicateAnswer,
         "answer_rejected.duplicate_answer"},
        {AnswerRejection::AnswerDoesNotFit,
         MessageKey::AnswerRejectedAnswerDoesNotFit,
         "answer_rejected.answer_does_not_fit"},
        {AnswerRejection::RatioUnchanged,
         MessageKey::AnswerRejectedRatioUnchanged,
         "answer_rejected.ratio_unchanged"},
        {AnswerRejection::RatioOutOfRange,
         MessageKey::AnswerRejectedRatioOutOfRange,
         "answer_rejected.ratio_out_of_range"},
        {AnswerRejection::PreviewMismatch,
         MessageKey::AnswerRejectedPreviewMismatch,
         "answer_rejected.preview_mismatch"},
        {AnswerRejection::EffectiveDateWithoutUnits,
         MessageKey::AnswerRejectedEffectiveDateWithoutUnits,
         "answer_rejected.effective_date_without_units"},
        {AnswerRejection::LaterSaleTooLarge,
         MessageKey::AnswerRejectedLaterSaleTooLarge,
         "answer_rejected.later_sale_too_large"},
        {AnswerRejection::CorrectionNotPositive,
         MessageKey::AnswerRejectedCorrectionNotPositive,
         "answer_rejected.correction_not_positive"},
        {AnswerRejection::CorrectionAboveOpenUnits,
         MessageKey::AnswerRejectedCorrectionAboveOpenUnits,
         "answer_rejected.correction_above_open_units"},
        {AnswerRejection::CorrectionTooLarge,
         MessageKey::AnswerRejectedCorrectionTooLarge,
         "answer_rejected.correction_too_large"},
        {AnswerRejection::CorrectionOverBudget,
         MessageKey::AnswerRejectedCorrectionOverBudget,
         "answer_rejected.correction_over_budget"},
    };

    for (const Row& row : rows)
    {
        const std::string json = serializeRejectionsJson(
            {rejection(0, row.mReason, row.mKey, {textParam("requestId", "request-0001")})});

        EXPECT_EQ(json,
                  R"({"rejections":[{"index":0,"messageKey":")" + row.mExpectedKey +
                      R"(","messageParams":{"requestId":"request-0001"}}]})");
    }
}

TEST(RejectionJsonTest, DeleteRejectionsNameTheDecisionId) {
    const std::string json =
        serializeRejectionsJson({rejection(0,
                                           AnswerRejection::StaleRevision,
                                           MessageKey::AnswerRejectedStaleRevisionDelete,
                                           {textParam("decisionId", "decision-0001")}),
                                 rejection(1,
                                           AnswerRejection::UnknownDecision,
                                           MessageKey::AnswerRejectedUnknownDecision,
                                           {textParam("decisionId", "decision-0099")}),
                                 rejection(2,
                                           AnswerRejection::DuplicateAnswer,
                                           MessageKey::AnswerRejectedDuplicateDelete,
                                           {textParam("decisionId", "decision-0002")})});

    EXPECT_EQ(json,
              R"({"rejections":[)"
              R"({"index":0,"messageKey":"answer_rejected.stale_revision_delete",)"
              R"("messageParams":{"decisionId":"decision-0001"}},)"
              R"({"index":1,"messageKey":"answer_rejected.unknown_decision",)"
              R"("messageParams":{"decisionId":"decision-0099"}},)"
              R"({"index":2,"messageKey":"answer_rejected.duplicate_delete",)"
              R"("messageParams":{"decisionId":"decision-0002"}}]})");
}

TEST(RejectionJsonTest, NoEntryHasAnEnglishMessage) {
    const std::string json =
        serializeRejectionsJson({rejection(0,
                                           AnswerRejection::StaleRevision,
                                           MessageKey::AnswerRejectedStaleRevision,
                                           {textParam("requestId", "request-0001")})});

    EXPECT_EQ(json.find("\"message\""), std::string::npos);
    EXPECT_EQ(json.find("English"), std::string::npos);
}

TEST(InputFaultJsonTest, RateFileFaultHasTheFileNameAndTheRow) {
    // The file name holds a Windows folder; the JSON keeps the name only.
    const RateFileFault fault{ParseDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = DiagnosticCode::InvalidValue,
        .mSourceFile = "C:\\data\\rates.csv",
        .mRowIndex = 3,
        .mMessage = "An English sentence.",
        .mMessageKey = MessageKey::InvalidValueRateDate,
    }};

    EXPECT_EQ(serializeInputFaultJson(fault),
              R"({"inputFault":{"kind":"ecb_rate_file","messageKey":"invalid_value.rate_date",)"
              R"("messageParams":{},"source":{"file":"rates.csv","row":3}}})");
}

TEST(InputFaultJsonTest, RateFileFaultOfTheWholeFileHasNoRow) {
    const RateFileFault fault{ParseDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = DiagnosticCode::ParseError,
        .mSourceFile = "rates.csv",
        .mMessage = "An English sentence.",
        .mMessageKey = MessageKey::ParseErrorUnreadableRateFile,
    }};

    EXPECT_EQ(serializeInputFaultJson(fault),
              R"({"inputFault":{"kind":"ecb_rate_file",)"
              R"("messageKey":"parse_error.unreadable_rate_file",)"
              R"("messageParams":{},"source":{"file":"rates.csv"}}})");
}

TEST(InputFaultJsonTest, ReferenceFileFaultHasTheFileNameAndTheKey) {
    const ReferenceFileFault fault{
        .mFile = "payers.csv",
        .mRow = 1,
        .mMessage = DiagnosticMessage{
            .mKey = MessageKey::ReferenceFileMissingColumns,
            .mParams = {numberParam("count", 2), textParam("columns", "country, source")},
            .mEnglish = "An English sentence.",
        }};

    EXPECT_EQ(parsed(serializeInputFaultJson(fault)),
              parsed(R"({"inputFault":{"kind":"reference_file",)"
                     R"("messageKey":"reference_file.missing_columns",)"
                     R"("messageParams":{"count":2,"columns":"country, source"},)"
                     R"("source":{"file":"payers.csv","row":1}}})"));
}

TEST(InputFaultJsonTest, NoPathAppearsInAnyFault) {
    const ReferenceFileFault fault{.mFile = "/home/someone/payers.csv",
                                   .mRow = std::nullopt,
                                   .mMessage = DiagnosticMessage{
                                       .mKey = MessageKey::ReferenceFileUnreadable,
                                       .mParams = {},
                                       .mEnglish = "An English sentence.",
                                   }};

    const std::string json = serializeInputFaultJson(fault);

    EXPECT_EQ(json.find("/home/"), std::string::npos);
    EXPECT_NE(json.find("payers.csv"), std::string::npos);
}

} // namespace
