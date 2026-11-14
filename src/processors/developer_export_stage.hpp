#pragma once

#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"

#include <cstddef>
#include <vector>

namespace taxbroker {

/// Where the three confirmations of the developer export stand.
enum class DeveloperConfirmation {
    Open,      ///< A step waits for an answer; its request is in the input.
    Cancelled, ///< The user cancelled a step: no unsafe output.
    Confirmed, ///< All three steps were confirmed for this list of investments.
};

class DeveloperExportStage final {
  public:
    /// Asks the three confirmations in order (history is missing, every name and ISIN with the
    /// risk, the explicit confirmation), one request per step, a later step only after the earlier
    /// one is confirmed. aLinkedDiagnostics are the processing diagnostics of the investments'
    /// `incomplete_history` errors. A cancel at any step gives Cancelled, so the caller asks the
    /// failure question again. The answers are bound to the exact list of aInvestments.
    [[nodiscard]] static DeveloperConfirmation
    confirm(ApplicationResultInput& aInput,
            RunDecisions& aDecisions,
            const std::vector<FailedInvestment>& aInvestments,
            const std::vector<SourceReference>& aSources,
            const std::vector<std::size_t>& aLinkedDiagnostics);
};

} // namespace taxbroker
