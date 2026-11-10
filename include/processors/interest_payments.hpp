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
    OtherInterestReportObr mOtherReport; ///< Rows in ledger order.
    /// The deposit question, the payer choices and the bond type questions.
    std::vector<RequestInput> mRequests;
    std::vector<ProcessingDiagnostic> mDiagnostics;
    std::vector<UsedOfficialRate> mUsedRates; ///< One per currency and day, by date, then code.
};

/// Sorts the reporting year's interest into Doh-DHO (deposit interest) and Doh-Obr (cash interest
/// and bond coupons) in EUR at the official rate of the receipt date. Payments of 0 or less are not
/// rows. A bond with a sale or maturity in the year asks its type; its coupons are not rows while
/// the type is open, discount or cancelled. A missing rate is an error of its payment.
[[nodiscard]] InterestPaymentsResult prepareInterestPayments(const MergedStatement& aStatement,
                                                             const ReportingYearLedger& aLedger,
                                                             const ReferenceCatalogue& aCatalogue,
                                                             const OfficialRateTable& aRates,
                                                             RunDecisions& aDecisions);

/// Removes the coupon rows of the listed ISINs from the report, in every broker. Broker cash
/// interest and the coupons of other ISINs stay, in their order.
void excludeCoupons(OtherInterestReportObr& aReport, const std::vector<Isin>& aIsins);

/// Removes the cash-interest rows of the listed brokers, the other half of a Doh-Obr exclusion.
void excludeBrokerCashInterest(OtherInterestReportObr& aReport,
                               const std::vector<Broker>& aBrokers);

/// Appends the diagnostics, the requests and the excluded items of the result to aInput, with
/// their positions moved to the numbering of aInput. The report models stay with the caller.
void appendInterestPayments(ApplicationResultInput& aInput, InterestPaymentsResult&& aResult);

} // namespace taxbroker
