#pragma once

#include "processors/fifo_matcher.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"

#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

/// Split rows of one ISIN from different brokers at most this many days apart may be one split
/// booked on different dates, so they block until the identity question exists.
inline constexpr int kDifferentDatesWindowDays = kBrokerDateMarginDays;

enum class CorporateActionState {
    Applied,
    RatioConfirmed,
    WaitingForRatio,
    WaitingForEarlierAction,
    Cancelled,
    /// The same broker has more than one action row of the ISIN on that day: one action or
    /// several, and in which order, is unknown.
    SameDayAmbiguous,
    /// Another broker has a split row of the ISIN on another date at most
    /// kDifferentDatesWindowDays away: one split or two is unknown.
    DifferentDatesAmbiguous,
    /// A merger, which the app does not support.
    Unsupported,
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
    /// The date of the broker's rows.
    Date mEffectiveDate;
    std::vector<StatementEventReference> mEvents;
    CorporateActionState mState{CorporateActionState::WaitingForRatio};
    /// Set only for Applied and RatioConfirmed. Supplied by the user, never by the broker.
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
    /// year. An action in aConfirmedActions that the pool applied gets a warning that the user
    /// supplied its ratio. The first action the pool stopped at is asked: its ratio and effective
    /// date, with the open units just before it for every date it may have. A recorded answer
    /// makes it RatioConfirmed. The later actions wait for it, whatever their ratio.
    ///
    /// Action rows of one ISIN and date from different brokers are one action with one question;
    /// two rows of the same broker on one date, or split rows of different brokers on different
    /// dates at most kDifferentDatesWindowDays apart, are ambiguous; a merger is unsupported.
    /// The stage never depends on the broker otherwise.
    [[nodiscard]] static CorporateActionResult
    resolve(const MergedStatement& aStatement,
            const ReportingYearLedger& aLedger,
            const FifoMatchingResult& aFifo,
            const std::vector<ConfirmedCorporateAction>& aConfirmedActions,
            RunDecisions& aDecisions);
};

/// The FIFO pools with their corporate actions applied, and the corporate-action stage result.
struct CapitalPools {
    FifoMatchingResult mFifo;
    CorporateActionResult mActions;
};

/// Matches the FIFO pools and resolves their corporate actions until no new answer is found: each
/// confirmed action is applied, the pool continues, and the next action of the ISIN is asked with
/// the position after the earlier one. The CLI and the debug tools use this: one FifoMatcher and
/// one CorporateActionResolver call would leave a confirmed action unapplied.
[[nodiscard]] CapitalPools matchCapitalPools(const MergedStatement& aStatement,
                                             const ReportingYearLedger& aLedger,
                                             RunDecisions& aDecisions);

} // namespace taxbroker
