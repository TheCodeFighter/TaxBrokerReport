#pragma once

#include "processors/tax_processor.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"

#include <cstddef>
#include <set>
#include <vector>

namespace taxbroker {

/// What the classification stage found for the trade instruments of a run.
struct TaxClassificationResult {
    /// The ISINs that a tax rule or the user left out of capital gains. Copy it to
    /// ReportingYearLedger::mLeftOutIsins before the capital stages run.
    std::set<Isin> mLeftOutIsins;
    std::vector<ProcessingDiagnostic> mDiagnostics;
    /// The classification questions; mProcessingDiagnostics count from mDiagnostics.
    std::vector<RequestInput> mRequests;
    /// The excluded items; mProcessingDiagnostics count from mDiagnostics.
    std::vector<ExcludedItemInput> mExcludedItems;
    /// Positions in the merge diagnostics of the class errors this stage took over.
    std::vector<std::size_t> mResolvedMergeDiagnostics;
};

class TaxClassifier final {
  public:
    /// Gives every trade instrument one capital treatment from its asset class. A bond and crypto
    /// are left out by a tax rule; an unknown class is asked, and the parse and merge class errors
    /// of that instrument are taken over here.
    [[nodiscard]] static TaxClassificationResult
    classify(const MergedStatement& aStatement,
             const ReportingYearLedger& aLedger,
             const std::vector<StatementMergeDiagnostic>& aMergeDiagnostics,
             RunDecisions& aDecisions);
};

/// Appends the result to aInput: the diagnostics, the requests, the excluded items and the
/// resolved merge diagnostics, with their positions moved to the numbering of aInput.
void appendClassification(ApplicationResultInput& aInput, TaxClassificationResult&& aResult);

} // namespace taxbroker
