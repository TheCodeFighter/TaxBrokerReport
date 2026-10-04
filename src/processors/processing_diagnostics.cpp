#include "taxbroker/processing_diagnostics.hpp"

#include <algorithm>
#include <utility>

namespace taxbroker {

ProcessingDiagnostic numericDiagnostic(NumericError aError, ProcessingContext aContext) {
    auto code = ProcessingDiagnosticCode::UnrepresentableValue;
    std::string message = "The value cannot be represented at the required precision or range.";

    switch (aError)
    {
    case NumericError::Overflow:
        code = ProcessingDiagnosticCode::ArithmeticOverflow;
        message = "The calculation exceeds the supported integer range.";
        break;
    case NumericError::InvalidExchangeRate:
        code = ProcessingDiagnosticCode::InvalidExchangeRate;
        message = "The exchange rate is invalid.";
        break;
    case NumericError::InvalidInput:
    case NumericError::InvalidDenominator:
    case NumericError::UnrepresentableValue:
        break;
    }

    if (aContext.mStage == PipelineStage::CorporateAction)
    {
        // At this stage any numeric failure means the action cannot safely preserve the position.
        code = ProcessingDiagnosticCode::CorporateActionUnrepresentable;
        message = "The corporate action cannot preserve the position and exact basis.";
    }

    // Normalize caller-supplied scope so repeated reports/sources and input order do not change it.
    std::sort(aContext.mAffectedReports.begin(), aContext.mAffectedReports.end());
    aContext.mAffectedReports.erase(
        std::unique(aContext.mAffectedReports.begin(), aContext.mAffectedReports.end()),
        aContext.mAffectedReports.end());
    std::sort(aContext.mSources.begin(), aContext.mSources.end(), StableSourceOrder{});
    aContext.mSources.erase(std::unique(aContext.mSources.begin(), aContext.mSources.end()),
                            aContext.mSources.end());

    return ProcessingDiagnostic{DiagnosticSeverity::Error, code, std::move(aContext), message};
}

} // namespace taxbroker
