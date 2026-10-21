#include "taxbroker/processing_diagnostics.hpp"
#include "taxbroker/diagnostic_messages.hpp"

#include <algorithm>
#include <utility>

namespace taxbroker {

ProcessingDiagnostic numericDiagnostic(NumericError aError, ProcessingContext aContext) {
    auto code = ProcessingDiagnosticCode::UnrepresentableValue;
    auto key = MessageKey::UnrepresentableValue;

    switch (aError)
    {
    case NumericError::Overflow:
        code = ProcessingDiagnosticCode::ArithmeticOverflow;
        key = MessageKey::ArithmeticOverflow;
        break;
    case NumericError::InvalidExchangeRate:
        code = ProcessingDiagnosticCode::InvalidExchangeRate;
        key = MessageKey::InvalidExchangeRate;
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
        key = MessageKey::CorporateActionUnrepresentable;
    }

    // Normalize caller-supplied scope so repeated reports/sources and input order do not change it.
    std::sort(aContext.mAffectedReports.begin(), aContext.mAffectedReports.end());
    aContext.mAffectedReports.erase(
        std::unique(aContext.mAffectedReports.begin(), aContext.mAffectedReports.end()),
        aContext.mAffectedReports.end());
    std::sort(aContext.mSources.begin(), aContext.mSources.end(), StableSourceOrder{});
    aContext.mSources.erase(std::unique(aContext.mSources.begin(), aContext.mSources.end()),
                            aContext.mSources.end());

    auto message = makeDiagnosticMessage(key);

    return ProcessingDiagnostic{DiagnosticSeverity::Error,
                                code,
                                std::move(aContext),
                                std::move(message.mEnglish),
                                message.mKey,
                                std::move(message.mParams)};
}

} // namespace taxbroker
