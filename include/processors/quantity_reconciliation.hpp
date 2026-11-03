#pragma once

#include "processors/fifo_matcher.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"

#include <optional>
#include <vector>

namespace taxbroker {

/// What the quantity stage found for the pools that stopped at a sale with a tiny shortage.
struct QuantityReconciliationResult {
    std::vector<QuantityCorrection> mNewCorrections;
    std::vector<ProcessingDiagnostic> mDiagnostics;
    /// The quantity questions; mProcessingDiagnostics count from mDiagnostics.
    std::vector<RequestInput> mRequests;
};

/// EUR per unit that values the units removed from a sale: the higher of the sale unit price and
/// the highest exact value per unit of the open lots, which have units. Absent (unknown) for a
/// sale that is not in EUR, because no official rate is applied to it yet. An unknown value is
/// never reconciled.
[[nodiscard]] std::optional<ExactRational> correctionUnitValue(const TradeTransaction& aSale,
                                                               const std::vector<FifoLot>& aLots);

/// A copy of the statement in which each corrected sale has its corrected units. The unit
/// evidence of the sale keeps the original source value.
[[nodiscard]] MergedStatement
withQuantityCorrections(MergedStatement aStatement,
                        const std::vector<QuantityCorrection>& aCorrections);

class QuantityReconciler final {
  public:
    /// Looks at each pool that stopped at a sale with too few open units. A shortage below
    /// kReconciliationLimitUnits, whose impact keeps the ISIN below the EUR 0.01 budget:
    ///
    /// - is reduced to the open units when the exact source decimals of the trades up to the sale
    ///   cover the exact sale units and no action changed the units (a rounding difference);
    /// - otherwise asks for a corrected quantity. The answer is a correction, a cancel keeps the
    ///   error.
    ///
    /// Every other pool keeps the incomplete_history error of the FIFO run. aFifo loses the error
    /// of a sale with a question, which has its own error with the question linked.
    /// aApplied are the corrections the FIFO run used; a sale in it is not corrected again.
    [[nodiscard]] static QuantityReconciliationResult
    reconcile(const MergedStatement& aStatement,
              const ReportingYearLedger& aLedger,
              FifoMatchingResult& aFifo,
              const std::vector<QuantityCorrection>& aApplied,
              RunDecisions& aDecisions);
};

} // namespace taxbroker
