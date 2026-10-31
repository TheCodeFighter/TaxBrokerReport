#pragma once

#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"

#include <vector>

namespace taxbroker {

class CapitalFailureResolution final {
  public:
    /// Asks, or applies the answer to, the choice for the capital errors that belong to one
    /// investment each: leave every listed investment out of Doh-KDVP, or write no Doh-KDVP.
    ///
    /// Runs on the diagnostics and requests of every capital stage, before the XML. With no
    /// answer it adds the question to aInput. With the exclusion it resolves the errors and drops
    /// the open questions of the listed ISINs, and returns those ISINs sorted. Otherwise it returns
    /// none. It asks nothing when an error that no investment owns (a file, a row, the year or the
    /// XML) also blocks capital gains.
    [[nodiscard]] static std::vector<Isin> resolve(ApplicationResultInput& aInput,
                                                   RunDecisions& aDecisions);
};

} // namespace taxbroker
