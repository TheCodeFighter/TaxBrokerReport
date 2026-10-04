#pragma once

#include "taxbroker/exact_arithmetic.hpp"

#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

enum class PipelineStage {
    Parsing,
    Merging,
    HistoricalValidation,
    CorporateAction,
    FifoTax,
    XmlGeneration,
};

enum class ReportType {
    CapitalGains,
    Dividends,
    BankDepositInterest,
    OtherInterest,
};

enum class ProcessingDiagnosticCode {
    UnrepresentableValue,
    ArithmeticOverflow,
    InvalidExchangeRate,
    CorporateActionUnrepresentable,
};

/// Caller-supplied failure scope and safe provenance; arithmetic helpers do not infer reports.
struct ProcessingContext {
    PipelineStage mStage{PipelineStage::FifoTax};
    std::vector<ReportType> mAffectedReports;
    std::vector<SourceReference> mSources;
    std::optional<Date> mTaxDate;
    std::optional<std::string> mField;
    std::optional<std::string> mInstrumentName;
    std::optional<Isin> mIsin;
};

/// Final application IDs and resolution status are assigned by later orchestration.
struct ProcessingDiagnostic {
    DiagnosticSeverity mSeverity{DiagnosticSeverity::Error};
    ProcessingDiagnosticCode mCode{ProcessingDiagnosticCode::UnrepresentableValue};
    ProcessingContext mContext;
    std::string mMessage;
};

/// Maps a numeric error to a scoped error diagnostic without embedding raw financial values.
/// Corporate-action context overrides the generic code; reports and sources are
/// sorted/deduplicated.
[[nodiscard]] ProcessingDiagnostic numericDiagnostic(NumericError aError,
                                                     ProcessingContext aContext);

} // namespace taxbroker
