#pragma once

#include "processors/capital_gains_report_builder.hpp"
#include "processors/official_rate_selection.hpp"

#include <vector>

namespace taxbroker {

class CapitalReportGates final {
  public:
    /// Turns every capital case that the walking skeleton does not support into an error
    /// for the capital report, so the report is never guessed. A pool stopped at a corporate
    /// action gets its diagnostics from CorporateActionResolver instead.
    /// A pool stopped at a foreign trade gets the error of that trade's missing rate from aRates.
    /// All inputs must come from the same run.
    [[nodiscard]] static std::vector<ProcessingDiagnostic>
    check(const MergedStatement& aStatement,
          const ReportingYearLedger& aLedger,
          const FifoMatchingResult& aFifo,
          const LossEligibilityResult& aLoss,
          const CapitalGainsBuildResult& aBuilt,
          const OfficialRateSelection& aRates);
};

} // namespace taxbroker
