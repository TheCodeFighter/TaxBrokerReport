#pragma once

#include "processors/fifo_matcher.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"

#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

enum class CorporateActionState {
    RatioConfirmed,
    WaitingForRatio,
    WaitingForEarlierAction,
    Cancelled,
    /// The same broker has more than one action row of the ISIN on that day: one action or
    /// several, and in which order, is unknown.
    SameDayAmbiguous,
    /// No open units just before the action, so there is nothing to ask about.
    NoPosition,
    /// The open units just before the action do not fit Units, so no question can be asked.
    NumericFailure,
    /// The class cannot reach Doh-KDVP (bond, derivative, crypto, private fund), so we skip it.
    NotAskedForClass,
};

/// One economic action: every action row of one ISIN on one date, one per broker.
struct ResolvedCorporateAction {
    Isin mIsin;
    std::string mName;
    Date mEffectiveDate;
    std::vector<StatementEventReference> mEvents;
    CorporateActionState mState{CorporateActionState::WaitingForRatio};
    /// Set only for RatioConfirmed. Supplied by the user, never by the broker.
    std::optional<SplitRatio> mRatio;
    std::optional<Date> mConfirmedDate;

    bool operator==(const ResolvedCorporateAction&) const = default;
};

struct CorporateActionResult {
    std::vector<ResolvedCorporateAction> mActions;
    std::vector<ProcessingDiagnostic> mDiagnostics;
    std::vector<RequestInput> mRequests;
};

class CorporateActionResolver final {
  public:
    /// Finds every corporate action up to 31 December of each ISIN with a sale in the selected
    /// year whose FIFO pool stopped at one, and asks for the ratio and effective date of the
    /// first, with the open units just before it and the dates that keep them. The later actions
    /// wait for it, whatever their ratio. Action rows of one ISIN and date from different brokers
    /// are one action with one question; two rows of the same broker on one date are ambiguous, on
    /// the first date or a later one. The stage never depends on the broker otherwise.
    [[nodiscard]] static CorporateActionResult resolve(const MergedStatement& aStatement,
                                                       const ReportingYearLedger& aLedger,
                                                       const FifoMatchingResult& aFifo,
                                                       RunDecisions& aDecisions);
};

} // namespace taxbroker
