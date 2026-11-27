#include "service/parse_failure.hpp"

#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/diagnostics.hpp"
#include "taxbroker/event_metadata.hpp"
#include "taxbroker/exception_kind.hpp"
#include "taxbroker/logging.hpp"

#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace taxbroker {

ParseResult parseFailureResult(const BrokerInputFile& aFile) {
    DiagnosticMessage message = makeDiagnosticMessage(MessageKey::ParseErrorInternal);
    // Only the last part of the name, like the parsers keep it (never a path).
    const std::optional<SourceFilename> sourceFile = SourceFilename::tryFromPath(aFile.mFile.mName);

    ParseResult result{.mBroker = aFile.mBroker};

    // The statement stays empty: a failed parser gives no events.
    result.mDiagnostics.push_back(ParseDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = DiagnosticCode::ParseError,
        .mSourceFile = sourceFile.has_value() ? sourceFile->value() : std::string{},
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    });

    return result;
}

ParseResult parseResultOfOutcome(TaskOutcome<ParseResult> aOutcome,
                                 const BrokerInputFile& aFile,
                                 const std::size_t aSourceIndex) {
    if (aOutcome.mValue.has_value())
    {
        return std::move(*aOutcome.mValue);
    }

    // Only a programming error gets here: the parsers handle their own failures.
    try
    { std::rethrow_exception(aOutcome.mException); } catch (...)
    { LOG_ERROR("Parser of source {} stopped ({})", aSourceIndex, currentExceptionKind()); }

    return parseFailureResult(aFile);
}

} // namespace taxbroker
