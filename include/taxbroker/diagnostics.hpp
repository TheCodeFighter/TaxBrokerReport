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

/// The kind of row a parser diagnostic is about. The application result uses it to decide which
/// reports the diagnostic affects. It is not part of parser JSON v1.
enum class ParsedRowKind {
    Trade,
    CorporateAction,
    Dividend,
    BondInterest,
    BrokerInterest,
    /// An interest row whose subtype could not be established.
    UnknownInterest,
    Benefit,
    PrivateMarket,
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
    /// Absent for a diagnostic of a file or a header, of a row of unknown or unsupported type, and
    /// of a record with the wrong field count.
    std::optional<ParsedRowKind> mRowKind;
};

} // namespace taxbroker
