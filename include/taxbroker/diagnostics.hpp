#pragma once

#include "taxbroker/diagnostic_messages.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

enum class DiagnosticSeverity {
    Warning,
    Error,
};

// These values form part of the frontend API contract. Add new values instead of renaming
// existing ones once a released frontend consumes them.
enum class DiagnosticCode {
    UnknownRowType,
    UnsupportedRowType,
    UnsupportedAssetClass,
    MissingField,
    InvalidValue,
    InconsistentValue,
    FieldCountMismatch,
    LocalizedNumberFormat,
    InvalidHeader,
    ParseError,
};

struct ParseDiagnostic {
    DiagnosticSeverity mSeverity{DiagnosticSeverity::Error};
    DiagnosticCode mCode{DiagnosticCode::ParseError};
    // Store only the filename. Exposing a host path is unnecessary for a local browser client.
    std::string mSourceFile;
    std::optional<std::size_t> mRowIndex;
    std::optional<std::string> mTransactionId;
    std::optional<std::string> mField;
    /// English sentence for logs, the CLI and developer tools; the frontend uses the key.
    std::string mMessage;
    /// Set for every diagnostic the code emits; empty only in tests that build a bare diagnostic.
    std::optional<MessageKey> mMessageKey;
    std::vector<MessageParam> mMessageParams;
};

} // namespace taxbroker
