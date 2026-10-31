#pragma once

#include "taxbroker/processing_diagnostics.hpp"
#include "taxbroker/statement_merger.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace taxbroker {

/// Inclusive bounds accepted by TaxProcessor::prepareReportingYear.
inline constexpr int kMinimumReportingYear = 2000;
inline constexpr int kMaximumReportingYear = 9999;

/// The 30-day loss window: the sale day with this many days before and after it, both ends
/// included.
inline constexpr int kLossWindowDays = 30;

struct TaxProcessingRequest {
    std::optional<int> mReportingYear;
    StatementMergeResult mMerged;
};

/// Candidate references into TaxProcessingResult::mMerged, not calculated report rows.
struct ReportingYearLedger {
    int mYear{};
    Date mYearStart;
    Date mYearEnd;
    Date mLossWindowStart;
    Date mLossWindowEnd;
    std::vector<StatementEventReference> mCapitalEvents;
    /// The prefix before 1 January; older sales/actions must also establish opening inventory.
    std::size_t mHistoricalCapitalEventCount{};
    std::vector<StatementEventReference> mSelectedYearEvents;
    std::vector<StatementEventReference> mLossAnalysisEvents;
    /// Source sales only. Classification and unresolved errors still control final no_data status.
    bool mHasSelectedYearSales{};
};

struct TaxProcessingResult {
    StatementMergeResult mMerged;
    /// Present on successful boundary preparation, even when every candidate view is empty.
    std::optional<ReportingYearLedger> mLedger;
    std::vector<ProcessingDiagnostic> mDiagnostics;
};

class TaxProcessor final {
  public:
    /// Accepts merger-produced chronology; preserves upstream diagnostics and all source data.
    /// Missing/invalid years return no ledger and an InvalidReportingYear error.
    [[nodiscard]] static TaxProcessingResult prepareReportingYear(TaxProcessingRequest aRequest);
};

} // namespace taxbroker
