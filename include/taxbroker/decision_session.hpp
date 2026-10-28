#pragma once

#include "taxbroker/application_result.hpp"
#include "taxbroker/diagnostic_messages.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace taxbroker {

class DecisionSession;

/// The answers a calculation may use. A stage looks up its own question.
class RunDecisions final {
  public:
    /// The answer to a question with the same type and question data, or nullptr. Marks the
    /// answer as used in this calculation.
    [[nodiscard]] const AnswerValue* find(const DecisionQuestion& aQuestion);

  private:
    friend class DecisionSession;

    /// Only what identifies the question; its sources are run-local and not kept.
    struct Entry {
        RequestType mType{};
        QuestionData mData;
        AnswerValue mAnswer;
        bool mFromEarlierRun{};
        bool mUsed{};
    };

    /// In recording order.
    std::vector<Entry> mEntries;
};

/// One complete calculation of a run from its full input. It must not keep state between calls.
using RunCalculation = std::function<ApplicationResultInput(RunDecisions&)>;

enum class AnswerRejection {
    StaleRevision,
    UnknownRequest,
    DuplicateAnswer,
    AnswerDoesNotFit,
    UnknownDecision,
    RatioNotPositive,
    RatioUnchanged,
    RatioOutOfRange,
    EffectiveDateOutOfRange,
    /// A ratio answer whose confirmed position differs from the preview of its ratio.
    PreviewMismatch,
};

struct RejectedAnswer {
    /// Position of the answer or of the deleted decision ID in the call.
    std::size_t mIndex{};
    AnswerRejection mReason{};
    /// English sentence for logs and the CLI.
    std::string mMessage;
    MessageKey mMessageKey{};
    std::vector<MessageParam> mMessageParams;
};

struct DecisionAnswer {
    std::string mRequestId;
    std::uint64_t mRunRevision{};
    AnswerValue mValue;
};

/// It writes nothing to disk.
///
/// The session changes only after a calculation succeeds. If the calculation or the result
/// builder throws, the exception passes on and the session stays as it was: the answers given or
/// deleted in that call are dropped, every other recorded answer stays, and the result and its
/// revision are the previous ones.
class DecisionSession final {
  public:
    /// Starts a run with new input. The recorded answers stay and are marked as from an earlier
    /// run.
    const ApplicationResult& start(RunCalculation aCalculation);

    [[nodiscard]] const ApplicationResult& result() const {
        return mResult;
    }

    /// Records the answers and recalculates; the revision goes up by 1. Every answer must name an
    /// open request of the current revision, once, with an answer that fits its type; a request
    /// without question data cannot be answered. A ratio answer needs two positive, different
    /// numbers, an effective date within the question's range and the position after the action
    /// that its preview gives; it is recorded reduced (20 / 10 as 2 / 1). If one answer does not
    /// fit, the whole call is rejected and nothing changes. No answers change nothing.
    [[nodiscard]] std::vector<RejectedAnswer> answer(std::vector<DecisionAnswer> aAnswers);

    /// Deletes the named recorded answers (IDs of the current result) and recalculates; the
    /// revision goes up by 1. All or nothing, like answer().
    [[nodiscard]] std::vector<RejectedAnswer>
    deleteAnswers(const std::vector<std::string>& aDecisionIds, std::uint64_t aRunRevision);

    /// Deletes every recorded answer and recalculates. Without recorded answers nothing changes.
    [[nodiscard]] std::vector<RejectedAnswer> deleteAllAnswers(std::uint64_t aRunRevision);

  private:
    /// One calculation with aDecisions, as the next revision.
    [[nodiscard]] ApplicationResult calculate(const RunCalculation& aCalculation,
                                              RunDecisions& aDecisions) const;

    /// Keeps the answers and the result of a calculation that succeeded.
    void commit(RunDecisions&& aDecisions, ApplicationResult&& aResult);

    RunCalculation mCalculation;
    RunDecisions mDecisions;
    std::uint64_t mLastRevision{};
    ApplicationResult mResult;
};

} // namespace taxbroker
