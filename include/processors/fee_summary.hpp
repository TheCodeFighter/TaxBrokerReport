#pragma once

#include "processors/tax_processor.hpp"
#include "taxbroker/application_result.hpp"

namespace taxbroker {

/// The informational summaries of the selected year: the fees and the benefits.
struct FeeAndBenefitSummary {
    FeeSummary mFees;
    BenefitSummary mBenefits;
};

/**
 * Adds up the fees and the benefits of the deduplicated events dated in the selected year, by
 * broker and original currency. A fee in another currency converts to EUR only with the broker
 * rate of its own event, when its direction is verified and its currency is the fee's; never with
 * an official rate. A benefit has no broker rate. An EUR total is set only when every amount in it
 * is in EUR or converted, and the sum fits; nothing here is a diagnostic, a request or a form
 * value.
 */
[[nodiscard]] FeeAndBenefitSummary summarizeFeesAndBenefits(const MergedStatement& aStatement,
                                                            const ReportingYearLedger& aLedger);

} // namespace taxbroker
