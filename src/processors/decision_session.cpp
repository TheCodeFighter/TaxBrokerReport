#include "taxbroker/decision_session.hpp"
#include "taxbroker/split_ratio.hpp"
#include "utils/date_utils.hpp"
#include "utils/numbered_id.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace taxbroker {

namespace {

/// A failure question takes a failure choice or a cancel, a loss question `no`, `yes` or a cancel,
/// and a ratio question a ratio or a cancel. A question without question data takes no answer
/// until its processor defines one, because all questions of its type would be the same question.
[[nodiscard]] bool fitsQuestion(const QuestionData& aData, const AnswerValue& aAnswer) {
    const auto fits = [&aAnswer](const auto& aData) {
        using Data = std::decay_t<decltype(aData)>;

        if constexpr (std::is_same_v<Data, std::monostate>)
        {
            return false;
        }
        else if constexpr (std::is_same_v<Data, FailureResolutionQuestion>)
        {
            return std::holds_alternative<FailureResolutionAnswer>(aAnswer) ||
                   std::holds_alternative<CancelAnswer>(aAnswer);
        }
        else if constexpr (std::is_same_v<Data, LossEligibilityQuestion>)
        {
            return std::holds_alternative<NoOtherActivityAnswer>(aAnswer) ||
                   std::holds_alternative<OtherActivityAnswer>(aAnswer) ||
                   std::holds_alternative<CancelAnswer>(aAnswer);
        }
        else
        {
            static_assert(std::is_same_v<Data, CorporateActionRatioQuestion>);
            return std::holds_alternative<SplitRatioAnswer>(aAnswer) ||
                   std::holds_alternative<CancelAnswer>(aAnswer);
        }
    };

    return std::visit(fits, aData);
}

[[nodiscard]] RejectedAnswer rejected(std::size_t aIndex,
                                      AnswerRejection aReason,
                                      MessageKey aKey,
                                      std::string aParamName,
                                      std::string aId) {
    auto message = makeDiagnosticMessage(aKey, {textParam(std::move(aParamName), std::move(aId))});

    return RejectedAnswer{
        .mIndex = aIndex,
        .mReason = aReason,
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    };
}

[[nodiscard]] RejectedAnswer rejectedAnswer(std::size_t aIndex,
                                            AnswerRejection aReason,
                                            MessageKey aKey,
                                            const std::string& aRequestId) {
    return rejected(aIndex, aReason, aKey, "requestId", aRequestId);
}

/// The rejection of a ratio answer that names the field to correct.
[[nodiscard]] RejectedAnswer
rejectedRatioField(std::size_t aIndex, const std::string& aRequestId, std::string aField) {
    auto message = makeDiagnosticMessage(
        MessageKey::AnswerRejectedRatioNotPositive,
        {textParam("requestId", aRequestId), textParam("field", std::move(aField))});

    return RejectedAnswer{
        .mIndex = aIndex,
        .mReason = AnswerRejection::RatioNotPositive,
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    };
}

/// The rejection of a ratio answer whose effective date is outside the question's range.
[[nodiscard]] RejectedAnswer rejectedEffectiveDate(std::size_t aIndex,
                                                   const std::string& aRequestId,
                                                   const CorporateActionRatioQuestion& aQuestion) {
    auto message = makeDiagnosticMessage(
        MessageKey::AnswerRejectedEffectiveDateOutOfRange,
        {textParam("requestId", aRequestId),
         textParam("earliestDate", formatCalendarDate(aQuestion.mEarliestDate)),
         textParam("latestDate", formatCalendarDate(aQuestion.mLatestDate))});

    return RejectedAnswer{
        .mIndex = aIndex,
        .mReason = AnswerRejection::EffectiveDateOutOfRange,
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    };
}

/// Checks a ratio answer: both numbers positive and not equal, the effective date within the
/// question's range, and the confirmed position equal to the preview of the ratio for the
/// question's position before the action.
[[nodiscard]] std::optional<RejectedAnswer> checkRatioAnswer(std::size_t aIndex,
                                                             const DecisionRequest& aRequest,
                                                             const SplitRatioAnswer& aAnswer) {
    const auto& question = std::get<CorporateActionRatioQuestion>(aRequest.mData);

    if (aAnswer.mNewShares <= 0)
    {
        return rejectedRatioField(aIndex, aRequest.mId, "newShares");
    }

    if (aAnswer.mOldShares <= 0)
    {
        return rejectedRatioField(aIndex, aRequest.mId, "oldShares");
    }

    if (aAnswer.mNewShares == aAnswer.mOldShares)
    {
        return rejectedAnswer(aIndex,
                              AnswerRejection::RatioUnchanged,
                              MessageKey::AnswerRejectedRatioUnchanged,
                              aRequest.mId);
    }

    // Within the range the position before the action is the same, so the preview still holds.
    if (aAnswer.mEffectiveDate < question.mEarliestDate ||
        aAnswer.mEffectiveDate > question.mLatestDate)
    {
        return rejectedEffectiveDate(aIndex, aRequest.mId, question);
    }

    // Both numbers are positive and different, so the ratio exists.
    const auto ratio = makeSplitRatio(aAnswer.mNewShares, aAnswer.mOldShares).value();
    const auto preview = previewSplitUnits(question.mPositionBefore, ratio);

    if (!preview)
    {
        return rejectedAnswer(aIndex,
                              AnswerRejection::RatioOutOfRange,
                              MessageKey::AnswerRejectedRatioOutOfRange,
                              aRequest.mId);
    }

    if (*preview != aAnswer.mConfirmedUnits)
    {
        return rejectedAnswer(aIndex,
                              AnswerRejection::PreviewMismatch,
                              MessageKey::AnswerRejectedPreviewMismatch,
                              aRequest.mId);
    }

    return std::nullopt;
}

[[nodiscard]] RejectedAnswer rejectedDelete(std::size_t aIndex,
                                            AnswerRejection aReason,
                                            MessageKey aKey,
                                            const std::string& aDecisionId) {
    return rejected(aIndex, aReason, aKey, "decisionId", aDecisionId);
}

} // namespace

const AnswerValue* RunDecisions::find(const DecisionQuestion& aQuestion) {
    const auto data = normalizedData(aQuestion.mData);
    const auto entry =
        std::find_if(mEntries.begin(), mEntries.end(), [&aQuestion, &data](const Entry& aEntry) {
            return aEntry.mType == aQuestion.mType && aEntry.mData == data;
        });

    if (entry == mEntries.end())
    {
        return nullptr;
    }

    entry->mUsed = true;

    return &entry->mAnswer;
}

const ApplicationResult& DecisionSession::start(RunCalculation aCalculation) {
    auto decisions = mDecisions;

    // Every answer recorded so far was given in an earlier run.
    for (auto& entry : decisions.mEntries)
    {
        entry.mFromEarlierRun = true;
    }

    auto calculated = calculate(aCalculation, decisions);

    mCalculation = std::move(aCalculation);
    commit(std::move(decisions), std::move(calculated));

    return mResult;
}

std::vector<RejectedAnswer> DecisionSession::answer(std::vector<DecisionAnswer> aAnswers) {
    std::vector<RejectedAnswer> rejections;
    std::vector<const DecisionRequest*> answered;

    for (std::size_t index = 0; index < aAnswers.size(); ++index)
    {
        const auto& given = aAnswers[index];

        if (given.mRunRevision != mResult.mRunRevision)
        {
            rejections.push_back(rejectedAnswer(index,
                                                AnswerRejection::StaleRevision,
                                                MessageKey::AnswerRejectedStaleRevision,
                                                given.mRequestId));
            continue;
        }

        const auto request = std::find_if(
            mResult.mRequests.begin(),
            mResult.mRequests.end(),
            [&given](const DecisionRequest& aRequest) { return aRequest.mId == given.mRequestId; });

        if (request == mResult.mRequests.end())
        {
            rejections.push_back(rejectedAnswer(index,
                                                AnswerRejection::UnknownRequest,
                                                MessageKey::AnswerRejectedUnknownRequest,
                                                given.mRequestId));
            continue;
        }

        const bool isRepeated =
            std::find(answered.begin(), answered.end(), &*request) != answered.end();

        if (isRepeated)
        {
            rejections.push_back(rejectedAnswer(index,
                                                AnswerRejection::DuplicateAnswer,
                                                MessageKey::AnswerRejectedDuplicateAnswer,
                                                given.mRequestId));
            continue;
        }

        answered.push_back(&*request);

        if (!fitsQuestion(request->mData, given.mValue))
        {
            rejections.push_back(rejectedAnswer(index,
                                                AnswerRejection::AnswerDoesNotFit,
                                                MessageKey::AnswerRejectedAnswerDoesNotFit,
                                                given.mRequestId));
            continue;
        }

        if (const auto* ratio = std::get_if<SplitRatioAnswer>(&given.mValue))
        {
            if (auto rejection = checkRatioAnswer(index, *request, *ratio))
            {
                rejections.push_back(std::move(*rejection));
            }
        }
    }

    if (!rejections.empty() || aAnswers.empty())
    {
        return rejections;
    }

    // A new answer to a recorded question (a cancelled one asked again) replaces the old one.
    auto decisions = mDecisions;
    auto& entries = decisions.mEntries;

    for (std::size_t index = 0; index < aAnswers.size(); ++index)
    {
        // The request data is already normalized.
        const auto& request = *answered[index];
        auto& value = aAnswers[index].mValue;

        // A checked ratio answer is kept reduced: 20 / 10 as 2 / 1.
        if (auto* ratio = std::get_if<SplitRatioAnswer>(&value))
        {
            const auto reduced = makeSplitRatio(ratio->mNewShares, ratio->mOldShares).value();

            ratio->mNewShares = reduced.mNewShares;
            ratio->mOldShares = reduced.mOldShares;
        }

        const auto recorded =
            std::find_if(entries.begin(), entries.end(), [&request](const auto& aEntry) {
                return aEntry.mType == request.mType && aEntry.mData == request.mData;
            });

        if (recorded != entries.end())
        {
            recorded->mAnswer = std::move(value);
            recorded->mFromEarlierRun = false;
            continue;
        }

        entries.push_back(
            {.mType = request.mType, .mData = request.mData, .mAnswer = std::move(value)});
    }

    auto calculated = calculate(mCalculation, decisions);

    commit(std::move(decisions), std::move(calculated));

    return rejections;
}

std::vector<RejectedAnswer>
DecisionSession::deleteAnswers(const std::vector<std::string>& aDecisionIds,
                               std::uint64_t aRunRevision) {
    std::vector<RejectedAnswer> rejections;
    std::vector<std::size_t> positions;

    for (std::size_t index = 0; index < aDecisionIds.size(); ++index)
    {
        const auto& id = aDecisionIds[index];

        if (aRunRevision != mResult.mRunRevision)
        {
            rejections.push_back(rejectedDelete(index,
                                                AnswerRejection::StaleRevision,
                                                MessageKey::AnswerRejectedStaleRevisionDelete,
                                                id));
            continue;
        }

        const auto& decisions = mResult.mDecisions;
        const auto decision =
            std::find_if(decisions.begin(),
                         decisions.end(),
                         [&id](const RecordedDecision& aDecision) { return aDecision.mId == id; });

        if (decision == decisions.end())
        {
            rejections.push_back(rejectedDelete(index,
                                                AnswerRejection::UnknownDecision,
                                                MessageKey::AnswerRejectedUnknownDecision,
                                                id));
            continue;
        }

        // mDecisions lists the entries in recording order, so the position is the entry.
        const auto position = static_cast<std::size_t>(decision - decisions.begin());

        if (std::find(positions.begin(), positions.end(), position) != positions.end())
        {
            rejections.push_back(rejectedDelete(index,
                                                AnswerRejection::DuplicateAnswer,
                                                MessageKey::AnswerRejectedDuplicateDelete,
                                                id));
            continue;
        }

        positions.push_back(position);
    }

    if (!rejections.empty() || aDecisionIds.empty())
    {
        return rejections;
    }

    // Erase from the back, so the earlier positions stay valid.
    std::sort(positions.rbegin(), positions.rend());

    auto decisions = mDecisions;

    for (const auto position : positions)
    {
        decisions.mEntries.erase(decisions.mEntries.begin() +
                                 static_cast<std::ptrdiff_t>(position));
    }

    auto calculated = calculate(mCalculation, decisions);

    commit(std::move(decisions), std::move(calculated));

    return rejections;
}

std::vector<RejectedAnswer> DecisionSession::deleteAllAnswers(std::uint64_t aRunRevision) {
    if (aRunRevision != mResult.mRunRevision)
    {
        return {rejectedDelete(0,
                               AnswerRejection::StaleRevision,
                               MessageKey::AnswerRejectedStaleRevisionDelete,
                               "all")};
    }

    if (mDecisions.mEntries.empty())
    {
        return {};
    }

    RunDecisions decisions;
    auto calculated = calculate(mCalculation, decisions);

    commit(std::move(decisions), std::move(calculated));

    return {};
}

ApplicationResult DecisionSession::calculate(const RunCalculation& aCalculation,
                                             RunDecisions& aDecisions) const {
    for (auto& entry : aDecisions.mEntries)
    {
        entry.mUsed = false;
    }

    auto input = aCalculation(aDecisions);
    input.mRunRevision = mLastRevision + 1;

    auto calculated = ApplicationResultBuilder::build(std::move(input));

    const auto& entries = aDecisions.mEntries;

    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        calculated.mDecisions.push_back(RecordedDecision{
            .mId = numberedId("decision", index + 1),
            .mType = entries[index].mType,
            .mData = entries[index].mData,
            .mAnswer = entries[index].mAnswer,
            .mFromEarlierRun = entries[index].mFromEarlierRun,
            .mUsed = entries[index].mUsed,
        });
    }

    return calculated;
}

void DecisionSession::commit(RunDecisions&& aDecisions, ApplicationResult&& aResult) {
    mDecisions = std::move(aDecisions);
    mResult = std::move(aResult);
    mLastRevision = mResult.mRunRevision;
}

} // namespace taxbroker
