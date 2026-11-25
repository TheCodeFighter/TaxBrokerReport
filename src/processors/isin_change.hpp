#pragma once

#include "processors/statement_merger.hpp"
#include "processors/tax_processor.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"
#include "taxbroker/processing_diagnostics.hpp"

#include <optional>
#include <vector>

namespace taxbroker {

struct IsinChangeResult {
    /// Absent when nothing changed. Otherwise the ledger must be prepared again from it.
    std::optional<MergedStatement> mStatement;
    std::vector<ProcessingDiagnostic> mDiagnostics;
    std::vector<RequestInput> mRequests;
};

/// Applies the ISIN changes that matter for the reporting year, each asked once. "Same issuer"
/// moves the old ISIN's trades to the new one; "not the same issuer" makes the rows a merger.
/// Unanswered or cancelled changes leave both ISINs with an error.
[[nodiscard]] IsinChangeResult applyIsinChanges(const MergedStatement& aStatement,
                                                const ReportingYearLedger& aLedger,
                                                RunDecisions& aDecisions);

} // namespace taxbroker
