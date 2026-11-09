#pragma once

#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"

#include <vector>

namespace taxbroker {

/// A form whose errors can belong to one investment each, and the question that asks about them.
struct FailureForm {
    ReportType mReport{};   ///< Always set by the creator.
    RequestType mRequest{}; ///< Always set by the creator.
};

/// Doh-KDVP: `capital_gains_failure_resolution_required`.
inline constexpr FailureForm kCapitalFailure{
    .mReport = ReportType::CapitalGains,
    .mRequest = RequestType::CapitalGainsFailureResolutionRequired,
};

/// Doh-Obr: `other_interest_failure_resolution_required`.
inline constexpr FailureForm kOtherInterestFailure{
    .mReport = ReportType::OtherInterest,
    .mRequest = RequestType::OtherInterestFailureResolutionRequired,
};

class FailureResolutionStage final {
  public:
    /// Asks, or applies the answer to, the choice for the errors of one form that belong to one
    /// investment each: leave every listed investment out of the form, or write no file for it.
    ///
    /// Runs on the diagnostics and requests of every stage of the form, before its XML. With no
    /// answer it adds the question to aInput. With the exclusion it resolves the errors and drops
    /// the open questions of the listed ISINs, and returns those ISINs sorted. Otherwise it returns
    /// none. It asks nothing when an error that no investment owns (a file, a row, the year or the
    /// XML) also blocks the form.
    [[nodiscard]] static std::vector<Isin>
    resolve(ApplicationResultInput& aInput, RunDecisions& aDecisions, const FailureForm& aForm);
};

} // namespace taxbroker
