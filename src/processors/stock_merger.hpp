#pragma once

#include "processors/statement_merger.hpp"
#include "processors/tax_processor.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"
#include "taxbroker/processing_diagnostics.hpp"

#include <optional>
#include <vector>

namespace taxbroker {

struct StockMergerResult {
    /// Absent when nothing changed. Otherwise the ledger must be prepared again from it.
    std::optional<MergedStatement> mStatement;
    std::vector<ProcessingDiagnostic> mDiagnostics;
    std::vector<RequestInput> mRequests;
};

/// Applies the mergers of two companies (shares of one ISIN become shares of another, with or
/// without cash) that matter for the reporting year, each asked once. A taxable answer sells the
/// old shares at the value of the new ones plus the cash and buys the new ones at that value on the
/// same date; a deferral carries the old lots to the new ISIN. Unanswered or cancelled mergers
/// leave both ISINs with an error; a merger with more than two ISINs stays unsupported.
[[nodiscard]] StockMergerResult applyStockMergers(const MergedStatement& aStatement,
                                                  const ReportingYearLedger& aLedger,
                                                  RunDecisions& aDecisions);

} // namespace taxbroker
