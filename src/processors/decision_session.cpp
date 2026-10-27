#include "taxbroker/decision_session.hpp"
#include "utils/numbered_id.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace taxbroker {

namespace {

/// A failure question takes a failure choice or a cancel. A question without question data
/// takes no answer until its processor defines one, because all questions of its type would be
/// the same question.
[[nodiscard]] bool fitsQuestion(const QuestionData& aData, const AnswerValue& aAnswer) {
    const auto fits = [&aAnswer](const auto& aData) {
        using Data = std::decay_t<decltype(aData)>;

        if constexpr (std::is_same_v<Data, std::monostate>)
        {
            return false;
        }
        else
        {
            static_assert(std::is_same_v<Data, FailureResolutionQuestion>);
            return std::holds_alternative<FailureResolutionAnswer>(aAnswer) ||
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
