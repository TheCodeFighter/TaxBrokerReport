#pragma once

#include <cstddef>
#include <optional>
#include <string>

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
    std::string mMessage;
};

} // namespace taxbroker
