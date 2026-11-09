#pragma once

#include "processors/dividend_payments.hpp"
#include "processors/interest_payments.hpp"
#include "processors/trade_conversion.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"
#include "taxbroker/income_report_models.hpp"
#include "taxbroker/reference_catalogue.hpp"

#include <vector>

namespace taxbroker {

/// The income models of the reporting year, after the exclusion of the failed ISINs.
struct IncomeReports {
    DividendReportDiv mDividends;
    DepositInterestReportDHO mDeposit;
    OtherInterestReportObr mOther;            ///< Broker cash interest and bond coupons.
    std::vector<UsedOfficialRate> mUsedRates; ///< One per currency and day, by date, then code.
};

/**
 * Runs the income classification, the dividend stage with its failure choice and the interest
 * stage with its failure choice, in that order, and appends what they found to aInput. It makes
 * no XML; each form keeps its own status in the application result built from aInput.
 */
[[nodiscard]] IncomeReports prepareIncomeReports(ApplicationResultInput& aInput,
                                                 const MergedStatement& aStatement,
                                                 const ReportingYearLedger& aLedger,
                                                 const ReferenceCatalogue& aCatalogue,
                                                 const OfficialRateTable& aRates,
                                                 RunDecisions& aDecisions);

} // namespace taxbroker
