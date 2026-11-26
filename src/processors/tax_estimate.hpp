#pragma once

#include "processors/income_report_models.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/report_models.hpp"

#include <array>
#include <optional>

namespace taxbroker {

/// The approximate tax of each form with data, without the form statuses. A year without rates
/// gives no forms; a form that cannot be summed is incomplete.
[[nodiscard]] TaxEstimate estimateTax(int aYear,
                                      const CapitalGainsReportKDVP& aCapital,
                                      const DividendReportDiv& aDividends,
                                      const std::optional<DepositInterestSummary>& aDeposit,
                                      const OtherInterestReportObr& aOther);

/// Adds the statuses (ReportType order) and the total: a form that is not generated has no amount,
/// and a tax up to EUR 10 counts 0.
[[nodiscard]] TaxEstimate completeTaxEstimate(TaxEstimate aEstimate,
                                              const std::array<ReportStatus, 4>& aStatuses);

} // namespace taxbroker
