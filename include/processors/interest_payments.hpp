#pragma once

#include "processors/trade_conversion.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"
#include "taxbroker/income_report_models.hpp"
#include "taxbroker/reference_catalogue.hpp"

#include <optional>
#include <vector>

namespace taxbroker {

struct InterestPaymentsResult {
    DepositInterestReportDHO mDepositReport; ///< Rows only when the allowance needs the form.
    /// Set when the user answered that the total stays at or below the allowance.
    std::optional<NoDataReason> mDepositNoDataReason;
    /// The year's imported qualifying interest in EUR, exact; none when no payment or a rate is
    /// missing.
    std::optional<Money> mQualifyingTotal;
    OtherInterestReportObr mOtherReport;           ///< Rows in ledger order.
    std::vector<ExcludedItemInput> mExcludedItems; ///< Slovenian coupon issuers, one per ISIN.
    std::vector<RequestInput> mRequests;           ///< The deposit question and the payer choices.
    std::vector<ProcessingDiagnostic> mDiagnostics;
    std::vector<UsedOfficialRate> mUsedRates; ///< One per currency and day, by date, then code.
};

/**
 * Sorts the interest of the reporting year into Doh-DHO (deposit interest) and Doh-Obr (cash
 * interest and bond coupons) by who held the cash on the receipt date, in EUR at the official rate
 * of that day. The deposit allowance of EUR 1,000 is compared with the exact total of the deposit
 * payments: above it the rows are made, at or below it the user is asked once whether the other
 * banks bring the total above it. A payment of 0 or less is never a row. Payers of coupons are
 * looked up by ISIN and Slovenian issuers are excluded; a payer without facts leaves them empty.
 */
[[nodiscard]] InterestPaymentsResult prepareInterestPayments(const MergedStatement& aStatement,
                                                             const ReportingYearLedger& aLedger,
                                                             const ReferenceCatalogue& aCatalogue,
                                                             const OfficialRateTable& aRates,
                                                             RunDecisions& aDecisions);

/// Appends the diagnostics, the requests and the excluded items of the result to aInput, with
/// their positions moved to the numbering of aInput. The report models stay with the caller.
void appendInterestPayments(ApplicationResultInput& aInput, InterestPaymentsResult&& aResult);

} // namespace taxbroker
