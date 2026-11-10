#pragma once

#include "processors/trade_conversion.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"
#include "taxbroker/income_report_models.hpp"
#include "taxbroker/reference_catalogue.hpp"

#include <vector>

namespace taxbroker {

struct DividendPaymentsResult {
    DividendReportDiv mReport;                     ///< Rows in date order, then ledger order.
    std::vector<ExcludedItemInput> mExcludedItems; ///< Excluded Slo payers, one per ISIN.
    std::vector<RequestInput> mRequests; ///< Payer and treaty text questions, none blocking.
    std::vector<ProcessingDiagnostic> mDiagnostics;
    std::vector<UsedOfficialRate> mUsedRates; ///< One per currency and day, by date, then code.
};

/**
 * Builds the Doh-Div rows of the reporting year in ledger order, in EUR at the official rate of
 * the receipt date. A blank tax is 0. Payers are looked up by ISIN; a missing one gives empty
 * payer facts. The taxed rows of a payer country with a treaty text in the reference files are
 * claimed with it; another country with foreign tax is asked for a text. Slovenian payers are
 * excluded. A payment without a rate has no row and an error of its ISIN. An Unknown currency
 * throws std::logic_error.
 */
[[nodiscard]] DividendPaymentsResult prepareDividendPayments(const MergedStatement& aStatement,
                                                             const ReportingYearLedger& aLedger,
                                                             const ReferenceCatalogue& aCatalogue,
                                                             const OfficialRateTable& aRates,
                                                             RunDecisions& aDecisions);

/// Removes every row of the listed ISINs, as the exclusion of failed ISINs asks. The order stays.
void excludeDividends(DividendReportDiv& aReport, const std::vector<Isin>& aExcluded);

/// Appends the diagnostics, the requests and the excluded items of the result to aInput, with
/// their positions moved to the numbering of aInput. The report model stays with the caller.
void appendDividendPayments(ApplicationResultInput& aInput, DividendPaymentsResult&& aResult);

} // namespace taxbroker
