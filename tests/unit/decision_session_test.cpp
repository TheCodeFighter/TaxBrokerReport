#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/decision_session.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace taxbroker;

constexpr auto kCapital = ReportType::CapitalGains;
constexpr auto kDividends = ReportType::Dividends;
constexpr auto kOther = ReportType::OtherInterest;

const AnswerValue kExclude = FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments};
const AnswerValue kFailReport = FailureResolutionAnswer{FailureResolution::FailReport};
const AnswerValue kCancel = CancelAnswer{};

/// What a stand-in stage got from RunDecisions in each calculation, in calculation order.
using SeenAnswers = std::vector<std::optional<AnswerValue>>;

DecisionQuestion capitalQuestion(std::vector<FailedInvestment> aInvestments) {
    return DecisionQuestion{
        .mType = RequestType::CapitalGainsFailureResolutionRequired,
        .mAffectedReports = {kCapital},
        .mData = FailureResolutionQuestion{.mInvestments = std::move(aInvestments)},
    };
}

DecisionQuestion otherInterestQuestion(std::vector<FailedInvestment> aInvestments) {
    return DecisionQuestion{
        .mType = RequestType::OtherInterestFailureResolutionRequired,
        .mAffectedReports = {kOther},
        .mData = FailureResolutionQuestion{.mInvestments = std::move(aInvestments)},
    };
}

ProcessingDiagnostic capitalIncompleteHistoryError(const Isin& aIsin, const std::string& aName) {
    return ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {kCapital},
                     .mInstrumentName = aName,
                     .mIsin = aIsin},
        .mMessage = "Synthetic capital error.",
    };
}

ProcessingDiagnostic otherInterestUnrepresentableError(const Isin& aIsin,
                                                       const std::string& aName) {
    return ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::UnrepresentableValue,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {kOther},
                     .mInstrumentName = aName,
                     .mIsin = aIsin},
        .mMessage = "Synthetic other-interest error.",
    };
}

std::optional<AnswerValue> copyOf(const AnswerValue* aAnswer) {
    return aAnswer != nullptr ? std::optional<AnswerValue>{*aAnswer} : std::nullopt;
}

// The first result and missing answers.

TEST(DecisionSessionTest, TheFirstResultIsRevisionOne) {
    // XX0000000001 has incomplete history, so capital gains asks the failure choice. Dividends
    // have XML.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};
        input.mReports[1].mXml = "<dividends/>";

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;

    const auto& result = session.start(calculation);

    EXPECT_EQ(result.mRunRevision, 1U);
    EXPECT_EQ(result.mStatus, OverallStatus::NeedsInput);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    EXPECT_FALSE(result.report(kCapital).mXml.has_value());
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mXml, "<dividends/>");
    EXPECT_TRUE(result.mDecisions.empty());
    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& request = result.mRequests[0];

    EXPECT_EQ(request.mId, "request-0001");
    EXPECT_EQ(request.mRunRevision, 1U);
    EXPECT_EQ(request.mType, RequestType::CapitalGainsFailureResolutionRequired);
    ASSERT_EQ(request.mInvestments.size(), 1U);

    const auto& investment = request.mInvestments[0];

    EXPECT_EQ(investment.mIsin, "XX0000000001");
    EXPECT_EQ(investment.mName, "Synthetic Share");
    EXPECT_EQ(
        investment.mReasonCodes,
        (std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::IncompleteHistory}));
    EXPECT_EQ(investment.mDiagnosticIds, (std::vector<std::string>{"diagnostic-0001"}));

    // One calculation, without any answer.
    ASSERT_EQ(seen.size(), 1U);
    EXPECT_FALSE(seen[0].has_value());
}

TEST(DecisionSessionTest, WithoutAnAnswerTheReportStaysNeedsInput) {
    // Capital gains asks the failure choice for XX0000000001 until it has a choice.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);

    const auto& result = session.result();

    // The session never answers by itself: no default choice, no second calculation.
    EXPECT_EQ(result.mRunRevision, 1U);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    EXPECT_FALSE(result.report(kCapital).mXml.has_value());
    EXPECT_EQ(seen.size(), 1U);
}

// Answers.

TEST(DecisionSessionTest, FailReportRecalculates) {
    // Capital gains asks the failure choice for XX0000000001 until it has a choice; the error
    // stays (applying a choice is #227). Dividends have XML.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};
        input.mReports[1].mXml = "<dividends/>";

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);

    const auto rejections = session.answer({{"request-0001", 1, kFailReport}});

    EXPECT_TRUE(rejections.empty());

    const auto& result = session.result();

    EXPECT_EQ(result.mRunRevision, 2U);
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mXml, "<dividends/>");
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_TRUE(result.mDiagnostics[0].mBlocking);

    // The second calculation got the answer for exactly that question.
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[1], kFailReport);
    EXPECT_NE(seen[1], kExclude);

    ASSERT_EQ(result.mDecisions.size(), 1U);

    const auto& decision = result.mDecisions[0];

    EXPECT_EQ(decision.mId, "decision-0001");
    EXPECT_EQ(decision.mAnswer, kFailReport);
    EXPECT_EQ(decision.mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_FALSE(decision.mFromEarlierRun);
    EXPECT_TRUE(decision.mUsed);
}

TEST(DecisionSessionTest, AnAnswerToAnEarlierRevisionIsRejected) {
    // Capital gains asks the failure choice for XX0000000001 until it has a choice.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}}).empty());

    const auto rejections = session.answer({{"request-0001", 1, kExclude}});

    ASSERT_EQ(rejections.size(), 1U);

    const auto& rejection = rejections[0];

    EXPECT_EQ(rejection.mIndex, 0U);
    EXPECT_EQ(rejection.mReason, AnswerRejection::StaleRevision);
    EXPECT_EQ(rejection.mMessageKey, MessageKey::AnswerRejectedStaleRevision);
    EXPECT_EQ(rejection.mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0001")}));
    EXPECT_EQ(rejection.mMessage,
              "The answer to request-0001 belongs to an earlier calculation. Answer the current "
              "question again.");

    // Nothing changed: no new revision, no new calculation, the recorded answer is the old one.
    EXPECT_EQ(session.result().mRunRevision, 2U);
    EXPECT_EQ(seen.size(), 2U);
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, kFailReport);
}

TEST(DecisionSessionTest, AnAnswerToAnUnknownRequestIsRejected) {
    // Capital gains asks the failure choice for XX0000000001: the only request is request-0001.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);

    const auto rejections = session.answer({{"request-0002", 1, kFailReport}});

    ASSERT_EQ(rejections.size(), 1U);

    EXPECT_EQ(rejections[0].mIndex, 0U);
    EXPECT_EQ(rejections[0].mReason, AnswerRejection::UnknownRequest);
    EXPECT_EQ(rejections[0].mMessageKey, MessageKey::AnswerRejectedUnknownRequest);
    EXPECT_EQ(rejections[0].mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0002")}));
    EXPECT_EQ(rejections[0].mMessage, "There is no open question request-0002.");

    EXPECT_EQ(session.result().mRunRevision, 1U);
    EXPECT_EQ(seen.size(), 1U);
}

TEST(DecisionSessionTest, TwoAnswersToOneRequestAreRejected) {
    // Capital gains asks the failure choice for XX0000000001 until it has a choice.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);

    const auto rejections =
        session.answer({{"request-0001", 1, kFailReport}, {"request-0001", 1, kExclude}});

    ASSERT_EQ(rejections.size(), 1U);

    EXPECT_EQ(rejections[0].mIndex, 1U);
    EXPECT_EQ(rejections[0].mReason, AnswerRejection::DuplicateAnswer);
    EXPECT_EQ(rejections[0].mMessageKey, MessageKey::AnswerRejectedDuplicateAnswer);
    EXPECT_EQ(rejections[0].mMessage, "Question request-0001 was answered more than once.");

    EXPECT_EQ(session.result().mRunRevision, 1U);
    EXPECT_TRUE(session.result().mDecisions.empty());
    EXPECT_EQ(seen.size(), 1U);
}

TEST(DecisionSessionTest, AQuestionWithoutQuestionDataCannotBeAnswered) {
    // Classification has no question data yet, so the questions for XX0000000001 and
    // XX0000000002 would be the same question: one answer would reach both. No answer fits,
    // not even a cancel.
    SeenAnswers seenShare;
    SeenAnswers seenFund;
    const auto calculation = [&seenShare, &seenFund](RunDecisions& aDecisions) {
        const DecisionQuestion share{
            .mType = RequestType::TaxClassificationRequired,
            .mAffectedReports = {kCapital},
            .mIsin = "XX0000000001",
        };
        const DecisionQuestion fund{
            .mType = RequestType::TaxClassificationRequired,
            .mAffectedReports = {kCapital},
            .mIsin = "XX0000000002",
        };

        seenShare.push_back(copyOf(aDecisions.find(share)));
        seenFund.push_back(copyOf(aDecisions.find(fund)));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mRequests = {RequestInput{.mQuestion = share}, RequestInput{.mQuestion = fund}};

        return input;
    };
    DecisionSession session;
    session.start(calculation);

    const auto failReport = session.answer({{"request-0001", 1, kFailReport}});
    const auto cancel = session.answer({{"request-0001", 1, kCancel}});

    ASSERT_EQ(failReport.size(), 1U);

    EXPECT_EQ(failReport[0].mReason, AnswerRejection::AnswerDoesNotFit);
    EXPECT_EQ(failReport[0].mMessageKey, MessageKey::AnswerRejectedAnswerDoesNotFit);
    EXPECT_EQ(failReport[0].mMessage, "This answer does not fit question request-0001.");

    ASSERT_EQ(cancel.size(), 1U);

    EXPECT_EQ(cancel[0].mReason, AnswerRejection::AnswerDoesNotFit);
    EXPECT_EQ(cancel[0].mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0001")}));

    // Nothing was recorded or recalculated; both questions are still open.
    const auto& result = session.result();

    EXPECT_EQ(result.mRunRevision, 1U);
    EXPECT_TRUE(result.mDecisions.empty());
    EXPECT_EQ(result.mRequests.size(), 2U);
    EXPECT_EQ(seenShare.size(), 1U);
    EXPECT_EQ(seenFund.size(), 1U);
}

TEST(DecisionSessionTest, OneBadAnswerRejectsTheWholeBatch) {
    // Capital gains asks the failure choice for XX0000000001 and other interest for
    // XX0000000002, each until it has a choice.
    SeenAnswers seenCapital;
    SeenAnswers seenOther;
    const auto calculation = [&seenCapital, &seenOther](RunDecisions& aDecisions) {
        const auto capital = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto other = otherInterestQuestion({{
            .mIsin = "XX0000000002",
            .mName = "Synthetic Bond",
            .mReasonCodes = {ProcessingDiagnosticCode::UnrepresentableValue},
        }});
        const auto* capitalAnswer = aDecisions.find(capital);
        const auto* otherAnswer = aDecisions.find(other);

        seenCapital.push_back(copyOf(capitalAnswer));
        seenOther.push_back(copyOf(otherAnswer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share"),
            otherInterestUnrepresentableError("XX0000000002", "Synthetic Bond")};

        if (capitalAnswer == nullptr)
        {
            input.mRequests.push_back(
                RequestInput{.mQuestion = capital, .mProcessingDiagnostics = {0}});
        }

        if (otherAnswer == nullptr)
        {
            input.mRequests.push_back(
                RequestInput{.mQuestion = other, .mProcessingDiagnostics = {1}});
        }

        return input;
    };
    DecisionSession session;
    const auto& first = session.start(calculation);

    // Capital gains comes first in the report order.
    ASSERT_EQ(first.mRequests.size(), 2U);
    EXPECT_EQ(first.mRequests[0].mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_EQ(first.mRequests[1].mType, RequestType::OtherInterestFailureResolutionRequired);

    const auto rejections =
        session.answer({{"request-0001", 1, kFailReport}, {"request-0003", 1, kFailReport}});

    ASSERT_EQ(rejections.size(), 1U);

    EXPECT_EQ(rejections[0].mIndex, 1U);
    EXPECT_EQ(rejections[0].mReason, AnswerRejection::UnknownRequest);

    // The valid first answer was not applied either.
    const auto& result = session.result();

    EXPECT_EQ(result.mRunRevision, 1U);
    EXPECT_EQ(result.mRequests.size(), 2U);
    EXPECT_TRUE(result.mDecisions.empty());
    EXPECT_EQ(seenCapital.size(), 1U);
    EXPECT_EQ(seenOther.size(), 1U);
}

TEST(DecisionSessionTest, AReusedRequestIdDoesNotAuthorizeANewQuestion) {
    // Capital gains asks the failure choice for XX0000000001 and other interest for
    // XX0000000002, each until it has a choice.
    SeenAnswers seenCapital;
    SeenAnswers seenOther;
    const auto calculation = [&seenCapital, &seenOther](RunDecisions& aDecisions) {
        const auto capital = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto other = otherInterestQuestion({{
            .mIsin = "XX0000000002",
            .mName = "Synthetic Bond",
            .mReasonCodes = {ProcessingDiagnosticCode::UnrepresentableValue},
        }});
        const auto* capitalAnswer = aDecisions.find(capital);
        const auto* otherAnswer = aDecisions.find(other);

        seenCapital.push_back(copyOf(capitalAnswer));
        seenOther.push_back(copyOf(otherAnswer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share"),
            otherInterestUnrepresentableError("XX0000000002", "Synthetic Bond")};

        if (capitalAnswer == nullptr)
        {
            input.mRequests.push_back(
                RequestInput{.mQuestion = capital, .mProcessingDiagnostics = {0}});
        }

        if (otherAnswer == nullptr)
        {
            input.mRequests.push_back(
                RequestInput{.mQuestion = other, .mProcessingDiagnostics = {1}});
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}}).empty());

    // Now request-0001 is the other-interest question.
    const auto& second = session.result();

    ASSERT_EQ(second.mRequests.size(), 1U);
    EXPECT_EQ(second.mRequests[0].mId, "request-0001");
    EXPECT_EQ(second.mRequests[0].mType, RequestType::OtherInterestFailureResolutionRequired);

    const auto rejections = session.answer({{"request-0001", 1, kFailReport}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections[0].mReason, AnswerRejection::StaleRevision);

    // The capital answer never reached the other-interest question.
    const auto& result = session.result();

    EXPECT_EQ(result.mRunRevision, 2U);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(seenOther.size(), 2U);
    EXPECT_FALSE(seenOther[1].has_value());
    ASSERT_EQ(seenCapital.size(), 2U);
    EXPECT_EQ(seenCapital[1], kFailReport);
}

TEST(DecisionSessionTest, AChangedFailedListNeedsANewAnswer) {
    // A stand-in where excluding the bond adds XX0000000003 to the capital list, as a cancelled
    // loss question can add an ISIN (#220).
    SeenAnswers seenCapital;
    const auto calculation = [&seenCapital](RunDecisions& aDecisions) {
        const auto other = otherInterestQuestion({{
            .mIsin = "XX0000000002",
            .mName = "Synthetic Bond",
            .mReasonCodes = {ProcessingDiagnosticCode::UnrepresentableValue},
        }});
        const auto* otherAnswer = aDecisions.find(other);
        const bool addsThird = otherAnswer != nullptr && *otherAnswer == kExclude;

        std::vector<FailedInvestment> failed{{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }};
        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share"),
            otherInterestUnrepresentableError("XX0000000002", "Synthetic Bond")};
        RequestInput capitalRequest{.mProcessingDiagnostics = {0}};

        if (addsThird)
        {
            failed.push_back({
                .mIsin = "XX0000000003",
                .mName = "Synthetic Fund",
                .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
            });
            input.mProcessingDiagnostics.push_back(
                capitalIncompleteHistoryError("XX0000000003", "Synthetic Fund"));
            capitalRequest.mProcessingDiagnostics.push_back(2);
        }

        const auto capital = capitalQuestion(failed);
        const auto* capitalAnswer = aDecisions.find(capital);

        seenCapital.push_back(copyOf(capitalAnswer));

        if (capitalAnswer == nullptr)
        {
            capitalRequest.mQuestion = capital;
            input.mRequests.push_back(capitalRequest);
        }

        if (otherAnswer == nullptr)
        {
            input.mRequests.push_back(
                RequestInput{.mQuestion = other, .mProcessingDiagnostics = {1}});
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);

    const auto rejections =
        session.answer({{"request-0001", 1, kExclude}, {"request-0002", 1, kExclude}});

    EXPECT_TRUE(rejections.empty());

    // The capital list changed, so the recorded exclusion for XX0000000001 alone does not apply.
    const auto& second = session.result();

    EXPECT_EQ(second.mRunRevision, 2U);
    EXPECT_EQ(second.report(kCapital).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(second.mRequests.size(), 1U);

    const auto& request = second.mRequests[0];

    EXPECT_EQ(request.mId, "request-0001");
    ASSERT_EQ(request.mInvestments.size(), 2U);
    EXPECT_EQ(request.mInvestments[0].mIsin, "XX0000000001");
    EXPECT_EQ(request.mInvestments[1].mIsin, "XX0000000003");
    ASSERT_EQ(seenCapital.size(), 2U);
    EXPECT_FALSE(seenCapital[1].has_value());

    // Both answers stay recorded; only the bond answer was used.
    ASSERT_EQ(second.mDecisions.size(), 2U);
    EXPECT_FALSE(second.mDecisions[0].mUsed);
    EXPECT_TRUE(second.mDecisions[1].mUsed);

    ASSERT_TRUE(session.answer({{"request-0001", 2, kFailReport}}).empty());

    // The two-ISIN question got fail_report, never the old exclusion.
    ASSERT_EQ(seenCapital.size(), 3U);
    EXPECT_EQ(seenCapital[2], kFailReport);
    ASSERT_EQ(session.result().mDecisions.size(), 3U);

    // Deleting the bond answer brings back the one-ISIN list, and with it its recorded answer.
    ASSERT_TRUE(session.deleteAnswers({"decision-0002"}, 3).empty());

    const auto& fourth = session.result();

    EXPECT_EQ(fourth.mRunRevision, 4U);
    ASSERT_EQ(seenCapital.size(), 4U);
    EXPECT_EQ(seenCapital[3], kExclude);
    ASSERT_EQ(fourth.mDecisions.size(), 2U);
    EXPECT_EQ(fourth.mDecisions[0].mAnswer, kExclude);
    EXPECT_TRUE(fourth.mDecisions[0].mUsed);
    EXPECT_EQ(fourth.mDecisions[1].mAnswer, kFailReport);
    EXPECT_FALSE(fourth.mDecisions[1].mUsed);
    // The bond question is open again.
    ASSERT_EQ(fourth.mRequests.size(), 1U);
    EXPECT_EQ(fourth.mRequests[0].mType, RequestType::OtherInterestFailureResolutionRequired);
}

TEST(DecisionSessionTest, CancellingAFailureChoiceKeepsTheQuestionOpen) {
    // Capital gains asks the failure choice for XX0000000001 until it has a choice; a cancelled
    // question is asked again. Dividends have XML.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};
        input.mReports[1].mXml = "<dividends/>";

        if (answer == nullptr || std::holds_alternative<CancelAnswer>(*answer))
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);

    const auto rejections = session.answer({{"request-0001", 1, kCancel}});

    EXPECT_TRUE(rejections.empty());

    // Cancel is neither exclusion nor failure: the same question is open and there is no XML.
    const auto& result = session.result();

    EXPECT_EQ(result.mRunRevision, 2U);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    EXPECT_FALSE(result.report(kCapital).mXml.has_value());
    EXPECT_EQ(result.report(kDividends).mXml, "<dividends/>");
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mId, "request-0001");
    ASSERT_EQ(result.mRequests[0].mInvestments.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mInvestments[0].mIsin, "XX0000000001");
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[1], kCancel);

    // A later choice replaces the cancellation.
    ASSERT_TRUE(session.answer({{"request-0001", 2, kFailReport}}).empty());

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, kFailReport);
    EXPECT_EQ(session.result().report(kCapital).mStatus, ReportStatus::Failed);
}

// Recorded answers across runs.

TEST(DecisionSessionTest, ANewRunUsesTheRecordedAnswerVisibly) {
    // Both runs: capital gains asks the failure choice for XX0000000001 until it has a choice.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}}).empty());

    const auto& result = session.start(calculation);

    EXPECT_EQ(result.mRunRevision, 3U);
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
    ASSERT_EQ(seen.size(), 3U);
    EXPECT_EQ(seen[2], kFailReport);

    // The answer is listed, so it is a visible default, not a silent one.
    ASSERT_EQ(result.mDecisions.size(), 1U);

    const auto& decision = result.mDecisions[0];

    EXPECT_EQ(decision.mId, "decision-0001");
    EXPECT_EQ(decision.mAnswer, kFailReport);
    EXPECT_TRUE(decision.mFromEarlierRun);
    EXPECT_TRUE(decision.mUsed);

    const auto rejections = session.answer({{"request-0001", 2, kFailReport}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections[0].mReason, AnswerRejection::StaleRevision);
}

TEST(DecisionSessionTest, ANewRunWithAnotherFileOrderStillMatches) {
    // The same question with other sources: first file, row 3, then second file, row 5.
    const auto run = [](std::size_t aSourceIndex, std::size_t aRow, SeenAnswers& aSeen) {
        return [aSourceIndex, aRow, &aSeen](RunDecisions& aDecisions) {
            auto question = capitalQuestion({{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
            }});
            question.mSources = {SourceReference{
                .mBroker = Broker::TradeRepublic,
                .mFilename = SourceFilename::fromPath("synthetic.csv"),
                .mSourceRow = aRow,
                .mInputSequence = {.mSourceIndex = aSourceIndex, .mEventIndex = aRow - 2},
            }};
            const auto* answer = aDecisions.find(question);

            aSeen.push_back(copyOf(answer));

            ApplicationResultInput input{.mReportingYear = 2024};

            if (answer == nullptr)
            {
                input.mRequests = {RequestInput{.mQuestion = question}};
            }

            return input;
        };
    };
    SeenAnswers seen;
    DecisionSession session;
    session.start(run(0, 3, seen));
    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}}).empty());

    const auto& result = session.start(run(1, 5, seen));

    EXPECT_TRUE(result.mRequests.empty());
    ASSERT_EQ(seen.size(), 3U);
    EXPECT_EQ(seen[2], kFailReport);
}

TEST(DecisionSessionTest, ANewRunWithAnotherListAsksAgain) {
    // The first run: capital gains asks the failure choice for XX0000000001 alone.
    const auto firstRun = [](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (aDecisions.find(question) == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    // The new input also lacks history for XX0000000002.
    const auto secondRun = [](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({
            {
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
            },
            {
                .mIsin = "XX0000000002",
                .mName = "Synthetic Fund",
                .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
            },
        });

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share"),
            capitalIncompleteHistoryError("XX0000000002", "Synthetic Fund")};

        if (aDecisions.find(question) == nullptr)
        {
            input.mRequests = {
                RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0, 1}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(firstRun);
    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}}).empty());

    const auto& result = session.start(secondRun);

    EXPECT_EQ(result.mRunRevision, 3U);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mId, "request-0001");
    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_TRUE(result.mDecisions[0].mFromEarlierRun);
    EXPECT_FALSE(result.mDecisions[0].mUsed);
}

TEST(DecisionSessionTest, AChoiceThatReplacesACancelFromAnEarlierRunIsFromThisRun) {
    // Both runs: capital gains asks the failure choice for XX0000000001 until it has a choice; a
    // cancelled question is asked again.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (answer == nullptr || std::holds_alternative<CancelAnswer>(*answer))
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(session.answer({{"request-0001", 1, kCancel}}).empty());

    // The new run uses the recorded cancel, so the question is asked again.
    const auto& second = session.start(calculation);

    EXPECT_EQ(second.mRunRevision, 3U);
    ASSERT_EQ(second.mRequests.size(), 1U);
    ASSERT_EQ(second.mDecisions.size(), 1U);

    EXPECT_EQ(second.mDecisions[0].mAnswer, kCancel);
    EXPECT_TRUE(second.mDecisions[0].mFromEarlierRun);
    EXPECT_TRUE(second.mDecisions[0].mUsed);

    const auto rejections = session.answer({{"request-0001", 3, kFailReport}});

    EXPECT_TRUE(rejections.empty());

    // The choice replaces the cancel under the same ID, and it was given in this run.
    const auto& result = session.result();

    EXPECT_EQ(result.mRunRevision, 4U);
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
    ASSERT_EQ(result.mDecisions.size(), 1U);

    const auto& decision = result.mDecisions[0];

    EXPECT_EQ(decision.mId, "decision-0001");
    EXPECT_EQ(decision.mAnswer, kFailReport);
    EXPECT_FALSE(decision.mFromEarlierRun);
    EXPECT_TRUE(decision.mUsed);

    ASSERT_EQ(seen.size(), 4U);
    EXPECT_EQ(seen[3], kFailReport);
}

TEST(DecisionSessionTest, TheOrderOfInvestmentsDoesNotChangeTheQuestion) {
    // The first calculation lists XX0000000002 first, every later one XX0000000001 first.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const FailedInvestment share{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        };
        const FailedInvestment fund{
            .mIsin = "XX0000000002",
            .mName = "Synthetic Fund",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        };
        const auto question =
            seen.empty() ? capitalQuestion({fund, share}) : capitalQuestion({share, fund});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question}};
        }

        return input;
    };
    DecisionSession session;

    const auto& first = session.start(calculation);

    ASSERT_EQ(first.mRequests.size(), 1U);
    ASSERT_EQ(first.mRequests[0].mInvestments.size(), 2U);
    EXPECT_EQ(first.mRequests[0].mInvestments[0].mIsin, "XX0000000001");
    EXPECT_EQ(first.mRequests[0].mInvestments[1].mIsin, "XX0000000002");

    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}}).empty());

    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[1], kFailReport);
}

TEST(DecisionSessionTest, OnlyTypeAndQuestionDataIdentifyARecordedAnswer) {
    // The first run asks the capital failure choice for XX0000000001, and it is answered.
    const auto firstRun = [](RunDecisions& aDecisions) {
        const DecisionQuestion question{
            .mType = RequestType::CapitalGainsFailureResolutionRequired,
            .mAffectedReports = {kCapital},
            .mIsin = "XX0000000001",
            .mData =
                FailureResolutionQuestion{
                    .mInvestments = {FailedInvestment{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Share",
                        .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                    }}},
        };
        ApplicationResultInput input{.mReportingYear = 2024};

        if (aDecisions.find(question) == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question}};
        }

        return input;
    };
    // The second run looks up five other questions and asks none of them.
    SeenAnswers seen;
    const auto secondRun = [&seen](RunDecisions& aDecisions) {
        // A tax date and no ISIN: where to look, not what is asked.
        const DecisionQuestion moved{
            .mType = RequestType::CapitalGainsFailureResolutionRequired,
            .mAffectedReports = {kCapital},
            .mTaxDate =
                Date{std::chrono::sys_days{std::chrono::year{2024} / 4 / 2}.time_since_epoch()},
            .mData =
                FailureResolutionQuestion{
                    .mInvestments = {FailedInvestment{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Share",
                        .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                    }}},
        };
        // The other form with the same list.
        const DecisionQuestion otherForm{
            .mType = RequestType::OtherInterestFailureResolutionRequired,
            .mAffectedReports = {kCapital},
            .mIsin = "XX0000000001",
            .mData =
                FailureResolutionQuestion{
                    .mInvestments = {FailedInvestment{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Share",
                        .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                    }}},
        };
        // Another reason for the same investment.
        const DecisionQuestion otherReason{
            .mType = RequestType::CapitalGainsFailureResolutionRequired,
            .mAffectedReports = {kCapital},
            .mIsin = "XX0000000001",
            .mData =
                FailureResolutionQuestion{
                    .mInvestments = {FailedInvestment{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Share",
                        .mReasonCodes = {ProcessingDiagnosticCode::LossEligibilityUnresolved},
                    }}},
        };
        // Another name for the same ISIN.
        const DecisionQuestion otherName{
            .mType = RequestType::CapitalGainsFailureResolutionRequired,
            .mAffectedReports = {kCapital},
            .mIsin = "XX0000000001",
            .mData =
                FailureResolutionQuestion{
                    .mInvestments = {FailedInvestment{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Fund",
                        .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                    }}},
        };
        // Another ISIN with the same name and reason.
        const DecisionQuestion otherIsin{
            .mType = RequestType::CapitalGainsFailureResolutionRequired,
            .mAffectedReports = {kCapital},
            .mIsin = "XX0000000001",
            .mData =
                FailureResolutionQuestion{
                    .mInvestments = {FailedInvestment{
                        .mIsin = "XX0000000002",
                        .mName = "Synthetic Share",
                        .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                    }}},
        };

        seen.push_back(copyOf(aDecisions.find(moved)));
        seen.push_back(copyOf(aDecisions.find(otherForm)));
        seen.push_back(copyOf(aDecisions.find(otherReason)));
        seen.push_back(copyOf(aDecisions.find(otherName)));
        seen.push_back(copyOf(aDecisions.find(otherIsin)));

        return ApplicationResultInput{.mReportingYear = 2024};
    };
    DecisionSession session;
    session.start(firstRun);
    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}}).empty());

    session.start(secondRun);

    // In order: moved, otherForm, otherReason, otherName, otherIsin.
    ASSERT_EQ(seen.size(), 5U);
    EXPECT_EQ(seen[0], kFailReport);
    EXPECT_FALSE(seen[1].has_value());
    EXPECT_FALSE(seen[2].has_value());
    EXPECT_FALSE(seen[3].has_value());
    EXPECT_FALSE(seen[4].has_value());
}

// Deleting answers.

TEST(DecisionSessionTest, DeletingAnAnswerAsksTheQuestionAgain) {
    // Capital gains asks the failure choice for XX0000000001 until it has a choice. Dividends
    // have XML.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};
        input.mReports[1].mXml = "<dividends/>";

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}}).empty());

    const auto rejections = session.deleteAnswers({"decision-0001"}, 2);

    EXPECT_TRUE(rejections.empty());

    const auto& result = session.result();

    EXPECT_EQ(result.mRunRevision, 3U);
    EXPECT_TRUE(result.mDecisions.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mId, "request-0001");
    EXPECT_EQ(result.report(kDividends).mXml, "<dividends/>");
    ASSERT_EQ(seen.size(), 3U);
    EXPECT_FALSE(seen[2].has_value());
}

TEST(DecisionSessionTest, DeletingAllAnswers) {
    // Capital gains asks the failure choice for XX0000000001 and other interest for
    // XX0000000002, each until it has a choice.
    SeenAnswers seenCapital;
    SeenAnswers seenOther;
    const auto calculation = [&seenCapital, &seenOther](RunDecisions& aDecisions) {
        const auto capital = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto other = otherInterestQuestion({{
            .mIsin = "XX0000000002",
            .mName = "Synthetic Bond",
            .mReasonCodes = {ProcessingDiagnosticCode::UnrepresentableValue},
        }});
        const auto* capitalAnswer = aDecisions.find(capital);
        const auto* otherAnswer = aDecisions.find(other);

        seenCapital.push_back(copyOf(capitalAnswer));
        seenOther.push_back(copyOf(otherAnswer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share"),
            otherInterestUnrepresentableError("XX0000000002", "Synthetic Bond")};

        if (capitalAnswer == nullptr)
        {
            input.mRequests.push_back(
                RequestInput{.mQuestion = capital, .mProcessingDiagnostics = {0}});
        }

        if (otherAnswer == nullptr)
        {
            input.mRequests.push_back(
                RequestInput{.mQuestion = other, .mProcessingDiagnostics = {1}});
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}, {"request-0002", 1, kFailReport}})
                    .empty());
    ASSERT_EQ(session.result().mDecisions.size(), 2U);

    const auto rejections = session.deleteAllAnswers(2);

    EXPECT_TRUE(rejections.empty());

    const auto& result = session.result();

    EXPECT_EQ(result.mRunRevision, 3U);
    EXPECT_TRUE(result.mDecisions.empty());
    EXPECT_EQ(result.mRequests.size(), 2U);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(seenCapital.size(), 3U);
    EXPECT_FALSE(seenCapital[2].has_value());
    ASSERT_EQ(seenOther.size(), 3U);
    EXPECT_FALSE(seenOther[2].has_value());
}

TEST(DecisionSessionTest, DeletingWithAnEarlierRevisionIsRejected) {
    // Capital gains asks the failure choice for XX0000000001 until it has a choice.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}}).empty());

    const auto rejections = session.deleteAnswers({"decision-0001"}, 1);

    ASSERT_EQ(rejections.size(), 1U);

    EXPECT_EQ(rejections[0].mIndex, 0U);
    EXPECT_EQ(rejections[0].mReason, AnswerRejection::StaleRevision);
    EXPECT_EQ(rejections[0].mMessageKey, MessageKey::AnswerRejectedStaleRevisionDelete);
    EXPECT_EQ(rejections[0].mMessageParams,
              (std::vector<MessageParam>{textParam("decisionId", "decision-0001")}));
    EXPECT_EQ(rejections[0].mMessage,
              "Deleting decision-0001 refers to an earlier calculation. Choose the answers to "
              "delete again.");

    const auto all = session.deleteAllAnswers(1);

    ASSERT_EQ(all.size(), 1U);

    EXPECT_EQ(all[0].mMessageKey, MessageKey::AnswerRejectedStaleRevisionDelete);
    EXPECT_EQ(all[0].mMessageParams, (std::vector<MessageParam>{textParam("decisionId", "all")}));

    EXPECT_EQ(session.result().mRunRevision, 2U);
    EXPECT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(seen.size(), 2U);
}

TEST(DecisionSessionTest, DeletingAnUnknownOrRepeatedAnswerIsRejected) {
    // Capital gains asks the failure choice for XX0000000001 until it has a choice.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}}).empty());

    const auto unknown = session.deleteAnswers({"decision-0002"}, 2);

    ASSERT_EQ(unknown.size(), 1U);

    EXPECT_EQ(unknown[0].mReason, AnswerRejection::UnknownDecision);
    EXPECT_EQ(unknown[0].mMessageKey, MessageKey::AnswerRejectedUnknownDecision);
    EXPECT_EQ(unknown[0].mMessage, "There is no saved answer decision-0002.");

    const auto repeated = session.deleteAnswers({"decision-0001", "decision-0001"}, 2);

    ASSERT_EQ(repeated.size(), 1U);

    EXPECT_EQ(repeated[0].mIndex, 1U);
    EXPECT_EQ(repeated[0].mReason, AnswerRejection::DuplicateAnswer);
    EXPECT_EQ(repeated[0].mMessageKey, MessageKey::AnswerRejectedDuplicateDelete);
    EXPECT_EQ(repeated[0].mMessage, "Answer decision-0001 was named more than once.");

    EXPECT_EQ(session.result().mRunRevision, 2U);
    EXPECT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(seen.size(), 2U);
}

TEST(DecisionSessionTest, CallsWithoutAnythingToDoChangeNothing) {
    // Capital gains asks the failure choice for XX0000000001; nothing is recorded yet.
    SeenAnswers seen;
    const auto calculation = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);

    EXPECT_TRUE(session.deleteAllAnswers(1).empty());
    EXPECT_TRUE(session.deleteAnswers({}, 1).empty());
    EXPECT_TRUE(session.answer({}).empty());

    EXPECT_EQ(session.result().mRunRevision, 1U);
    EXPECT_EQ(seen.size(), 1U);
}

// A calculation that fails.

TEST(DecisionSessionTest, AFailedCalculationDropsTheNewAnswers) {
    // Capital gains asks the failure choice for XX0000000001 until it has a choice; a cancelled
    // question is asked again. While `fails` is set, the calculation throws.
    bool fails = false;
    SeenAnswers seen;
    const auto calculation = [&fails, &seen](RunDecisions& aDecisions) {
        if (fails)
        {
            throw std::runtime_error("Synthetic calculation failure.");
        }

        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (answer == nullptr || std::holds_alternative<CancelAnswer>(*answer))
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    fails = true;

    EXPECT_THROW((void)session.answer({{"request-0001", 1, kFailReport}}), std::runtime_error);

    // The answer is dropped: the previous result, with its revision and open request, stays.
    const auto& afterFailure = session.result();

    EXPECT_EQ(afterFailure.mRunRevision, 1U);
    EXPECT_TRUE(afterFailure.mDecisions.empty());
    EXPECT_EQ(afterFailure.report(kCapital).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(afterFailure.mRequests.size(), 1U);
    EXPECT_EQ(afterFailure.mRequests[0].mId, "request-0001");

    // The next calculation never sees the dropped fail_report.
    fails = false;

    ASSERT_TRUE(session.answer({{"request-0001", 1, kCancel}}).empty());

    EXPECT_EQ(session.result().mRunRevision, 2U);
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[1], kCancel);
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, kCancel);
}

TEST(DecisionSessionTest, AFailedCalculationKeepsTheDeletedAnswers) {
    // Capital gains asks the failure choice for XX0000000001 and other interest for
    // XX0000000002, each until it has a choice. While `fails` is set, the calculation throws.
    bool fails = false;
    SeenAnswers seenCapital;
    SeenAnswers seenOther;
    const auto calculation = [&fails, &seenCapital, &seenOther](RunDecisions& aDecisions) {
        if (fails)
        {
            throw std::runtime_error("Synthetic calculation failure.");
        }

        const auto capital = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto other = otherInterestQuestion({{
            .mIsin = "XX0000000002",
            .mName = "Synthetic Bond",
            .mReasonCodes = {ProcessingDiagnosticCode::UnrepresentableValue},
        }});
        const auto* capitalAnswer = aDecisions.find(capital);
        const auto* otherAnswer = aDecisions.find(other);

        seenCapital.push_back(copyOf(capitalAnswer));
        seenOther.push_back(copyOf(otherAnswer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share"),
            otherInterestUnrepresentableError("XX0000000002", "Synthetic Bond")};

        if (capitalAnswer == nullptr)
        {
            input.mRequests.push_back(
                RequestInput{.mQuestion = capital, .mProcessingDiagnostics = {0}});
        }

        if (otherAnswer == nullptr)
        {
            input.mRequests.push_back(
                RequestInput{.mQuestion = other, .mProcessingDiagnostics = {1}});
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(
        session.answer({{"request-0001", 1, kFailReport}, {"request-0002", 1, kExclude}}).empty());
    fails = true;

    EXPECT_THROW((void)session.deleteAnswers({"decision-0001"}, 2), std::runtime_error);

    // Both answers stay, so decision-0002 is still the other-interest answer.
    const auto& afterFailure = session.result();

    EXPECT_EQ(afterFailure.mRunRevision, 2U);
    ASSERT_EQ(afterFailure.mDecisions.size(), 2U);
    EXPECT_EQ(afterFailure.mDecisions[0].mAnswer, kFailReport);
    EXPECT_EQ(afterFailure.mDecisions[1].mAnswer, kExclude);
    EXPECT_TRUE(afterFailure.mRequests.empty());

    fails = false;

    ASSERT_TRUE(session.deleteAnswers({"decision-0002"}, 2).empty());

    // Only the other-interest answer is gone; the capital answer still applies.
    const auto& result = session.result();

    EXPECT_EQ(result.mRunRevision, 3U);
    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_EQ(result.mDecisions[0].mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_EQ(result.mDecisions[0].mAnswer, kFailReport);
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::OtherInterestFailureResolutionRequired);
    ASSERT_EQ(seenCapital.size(), 3U);
    EXPECT_EQ(seenCapital[2], kFailReport);
}

TEST(DecisionSessionTest, AFailedCalculationKeepsAllAnswersWhenAllAreDeleted) {
    // Capital gains asks the failure choice for XX0000000001 until it has a choice. While
    // `fails` is set, the calculation throws.
    bool fails = false;
    SeenAnswers seen;
    const auto calculation = [&fails, &seen](RunDecisions& aDecisions) {
        if (fails)
        {
            throw std::runtime_error("Synthetic calculation failure.");
        }

        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}}).empty());
    fails = true;

    EXPECT_THROW((void)session.deleteAllAnswers(2), std::runtime_error);

    EXPECT_EQ(session.result().mRunRevision, 2U);
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, kFailReport);

    fails = false;

    ASSERT_TRUE(session.deleteAllAnswers(2).empty());

    EXPECT_EQ(session.result().mRunRevision, 3U);
    EXPECT_TRUE(session.result().mDecisions.empty());
    ASSERT_EQ(seen.size(), 3U);
    EXPECT_FALSE(seen[2].has_value());
}

TEST(DecisionSessionTest, AFailedNewRunKeepsThePreviousRun) {
    // The first run: capital gains asks the failure choice for XX0000000001 until it has a
    // choice. The second run throws.
    SeenAnswers seen;
    const auto firstRun = [&seen](RunDecisions& aDecisions) {
        const auto question = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto* answer = aDecisions.find(question);

        seen.push_back(copyOf(answer));

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share")};

        if (answer == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    const auto failingRun = [](RunDecisions&) -> ApplicationResultInput {
        throw std::runtime_error("Synthetic calculation failure.");
    };
    DecisionSession session;
    session.start(firstRun);
    ASSERT_TRUE(session.answer({{"request-0001", 1, kFailReport}}).empty());

    EXPECT_THROW((void)session.start(failingRun), std::runtime_error);

    // The answer is still from this run, not from an earlier one.
    EXPECT_EQ(session.result().mRunRevision, 2U);
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_FALSE(session.result().mDecisions[0].mFromEarlierRun);

    // The first run's calculation is still the one that recalculates.
    ASSERT_TRUE(session.deleteAnswers({"decision-0001"}, 2).empty());

    EXPECT_EQ(session.result().mRunRevision, 3U);
    ASSERT_EQ(seen.size(), 3U);
    EXPECT_FALSE(seen[2].has_value());
}

// Determinism and reminders.

TEST(DecisionSessionTest, TheSameAnswersGiveTheSameResult) {
    // Both sessions: capital gains asks the failure choice for XX0000000001 and other interest
    // for XX0000000002, each until it has a choice. Dividends have XML.
    const auto calculation = [](RunDecisions& aDecisions) {
        const auto capital = capitalQuestion({{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
        }});
        const auto other = otherInterestQuestion({{
            .mIsin = "XX0000000002",
            .mName = "Synthetic Bond",
            .mReasonCodes = {ProcessingDiagnosticCode::UnrepresentableValue},
        }});

        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {
            capitalIncompleteHistoryError("XX0000000001", "Synthetic Share"),
            otherInterestUnrepresentableError("XX0000000002", "Synthetic Bond")};
        input.mReports[1].mXml = "<dividends/>";

        if (aDecisions.find(capital) == nullptr)
        {
            input.mRequests.push_back(
                RequestInput{.mQuestion = capital, .mProcessingDiagnostics = {0}});
        }

        if (aDecisions.find(other) == nullptr)
        {
            input.mRequests.push_back(
                RequestInput{.mQuestion = other, .mProcessingDiagnostics = {1}});
        }

        return input;
    };
    DecisionSession first;
    DecisionSession second;
    first.start(calculation);
    second.start(calculation);

    ASSERT_TRUE(first.answer({{"request-0002", 1, kExclude}}).empty());
    ASSERT_TRUE(second.answer({{"request-0002", 1, kExclude}}).empty());

    const auto firstText = api::serializeApplicationResultJson(first.result());
    const auto secondText = api::serializeApplicationResultJson(second.result());

    EXPECT_EQ(firstText, secondText);
    EXPECT_NE(firstText.find(R"("runRevision":2)"), std::string::npos);
}

TEST(DecisionSessionTest, AWarningCreatesNoRequest) {
    DecisionSession session;

    const auto& result = session.start([](RunDecisions&) {
        ApplicationResultInput input{.mReportingYear = 2024};
        input.mProcessingDiagnostics = {ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Warning,
            .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
            .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic reminder.",
        }};
        input.mReports[0].mXml = "<kdvp/>";

        return input;
    });

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kCapital).mXml, "<kdvp/>");
    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);
}

} // namespace
