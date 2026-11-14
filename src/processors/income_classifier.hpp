#pragma once

#include "processors/tax_processor.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace taxbroker {

/// What the income classification stage found for the payments of a run.
struct IncomeClassificationResult {
    /// Statement for the dividend and interest stages; None means use the original statement.
    std::optional<MergedStatement> mStatement;
    std::vector<ProcessingDiagnostic> mDiagnostics;
    /// The classification questions; mProcessingDiagnostics count from mDiagnostics.
    std::vector<RequestInput> mRequests;
    /// The excluded items; mProcessingDiagnostics count from mDiagnostics.
    std::vector<ExcludedItemInput> mExcludedItems;
    /// Positions in the merge diagnostics of the interest-type errors this stage took over.
    std::vector<std::size_t> mResolvedMergeDiagnostics;
};

class IncomeClassifier final {
  public:
    /// Asks the tax treatment of each unclear interest or non-share dividend payment of the
    /// reporting year. Unanswered or cancelled payments block the forms they may belong to.
    [[nodiscard]] static IncomeClassificationResult
    classify(const MergedStatement& aStatement,
             const ReportingYearLedger& aLedger,
             const std::vector<StatementMergeDiagnostic>& aMergeDiagnostics,
             RunDecisions& aDecisions);
};

/// Appends the result to aInput like appendClassification: the diagnostics, the requests, the
/// excluded items and the resolved merge diagnostics, with positions moved to aInput's numbering.
void appendIncomeClassification(ApplicationResultInput& aInput,
                                IncomeClassificationResult&& aResult);

} // namespace taxbroker
