#pragma once

#include "taxbroker/application_result.hpp"
#include "taxbroker/application_run.hpp"
#include "taxbroker/decision_session.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace taxbroker::api {

/// The answer JSON does not fit its question (wrong form or type, not a whole number, a date that
/// does not exist). No detail.
struct AnswerShapeError {
    bool operator==(const AnswerShapeError&) const = default;
};

/// Why the body of an answer or delete call is unusable (`400`).
enum class CallBodyError {
    BadRequest,     ///< Not JSON, wrong field type or unknown field.
    PathNotAllowed, ///< A key named `path` anywhere in the body.
};

/// The error code for the frontend, e.g. `bad_request`.
[[nodiscard]] std::string_view callBodyErrorCode(CallBodyError aError);

/// Reads one answer by the type and question data of aRequest (forms in `docs/diagnostics.md`).
/// Only the shape is checked; the session rejects bad values.
[[nodiscard]] std::variant<AnswerValue, AnswerShapeError>
parseAnswerJson(std::string_view aAnswerJson, const DecisionRequest& aRequest);

/// The body of an answer call, read against the current result.
struct AnswerCall {
    std::vector<DecisionAnswer> mAnswers;
    /// Answers whose JSON does not fit their question, as `answer_does_not_fit` rejections.
    std::vector<RejectedAnswer> mShapeRejections;
};

/// Reads `{"runRevision":2,"answers":[...]}` against the current result.
[[nodiscard]] std::variant<AnswerCall, CallBodyError>
parseAnswerCallJson(std::string_view aBody, const ApplicationResult& aResult);

/// A delete body: `{"runRevision":3,"decisionIds":[...]}` or `{"runRevision":3,"all":true}`.
struct DeleteCall {
    std::uint64_t mRunRevision{};          ///< The revision the caller saw; always set.
    bool mAll{};                           ///< True: delete every answer.
    std::vector<std::string> mDecisionIds; ///< Empty when mAll is true.
};

[[nodiscard]] std::variant<DeleteCall, CallBodyError> parseDeleteCallJson(std::string_view aBody);

/// The 422 body of a rejected call: `{"rejections":[{"index":0,"messageKey":"..."}]}`, with
/// `messageParams` when there are any. No English text; the frontend builds it from the key.
[[nodiscard]] std::string serializeRejectionsJson(const std::vector<RejectedAnswer>& aRejections);

/// The 422 body of a run that cannot start (`inputFault`). The source holds the file name only,
/// never a path.
[[nodiscard]] std::string serializeInputFaultJson(const ApplicationInputFault& aFault);

} // namespace taxbroker::api
