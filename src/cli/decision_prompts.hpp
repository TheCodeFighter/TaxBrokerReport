#pragma once

#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"

#include <istream>
#include <ostream>
#include <string>

namespace taxbroker::cli {

/// Tells whether the command asks this question.
[[nodiscard]] bool canAsk(const QuestionData& aData);

/// Asks the open questions of the session one at a time, in request order, and passes each answer
/// on to the session, which checks it and calculates again. A rejected answer is printed and its
/// question is asked again. Stops when no question it can ask is open, or when the input ends;
/// the other questions stay open. Nothing is written to disk.
void askOpenQuestions(DecisionSession& aSession, std::istream& aIn, std::ostream& aOut);

/// One line about an answer the command recorded, for the summary.
[[nodiscard]] std::string answerLine(const RecordedDecision& aDecision);

} // namespace taxbroker::cli
