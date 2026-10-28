#pragma once

#include "processors/fifo_matcher.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"

#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

enum class LossSaleState {
    Decided,
    WaitingForAnswer,
    /// The user closed the ISIN's question without an answer.
    Cancelled,
    /// A corporate action after 31 December lies inside the window. Its units cannot be compared
    /// with the sale's units until splits are applied.
    CorporateActionInWindow,
};

/// The units of one purchase that the 30-day loss rule counts against one sale.
struct LossReplacementUse {
    StatementEventReference mPurchase;
    Date mDate;
    Units mUnits{};

    bool operator==(const LossReplacementUse&) const = default;
};

/// A sale of the selected year with at least one loss unit.
struct LossSaleEligibility {
    StatementEventReference mSale;
    Date mSaleDate;
    Date mWindowStart;
    Date mWindowEnd;
    /// Loss-making matches within the holding period.
    Units mLossUnits{};
    /// In the order they were taken, earliest purchase first.
    std::vector<LossReplacementUse> mReplacements;
    Units mKnownIneligibleUnits{};
    Units mDeclaredIneligibleUnits{};
    bool mAssumedBlocked{};
    LossSaleState mState{LossSaleState::WaitingForAnswer};
    std::optional<StatementEventReference> mCorporateAction;

    /// The loss units whose loss the 30-day rule blocks.
    [[nodiscard]] Units ineligibleUnits() const {
        return mKnownIneligibleUnits + mDeclaredIneligibleUnits;
    }

    bool operator==(const LossSaleEligibility&) const = default;
};

struct LossEligibilityIsin {
    Isin mIsin;
    std::string mName;
    std::vector<LossSaleEligibility> mSales;
};

enum class OpenLossQuestions {
    Ask,
    /// For a caller that does not ask.
    AssumeBlocked,
};

struct LossEligibilityResult {
    std::vector<LossEligibilityIsin> mIsins;
    std::vector<ProcessingDiagnostic> mDiagnostics;
    /// One question per ISIN.
    std::vector<RequestInput> mRequests;
};

class LossEligibilityChecker final {
  public:
    /// Applies the 30-day loss rule to the sales of the selected year, by quantity, as in the
    /// FURS examples.
    ///
    /// All inputs must come from the same run.
    [[nodiscard]] static LossEligibilityResult
    check(const MergedStatement& aStatement,
          const ReportingYearLedger& aLedger,
          const FifoMatchingResult& aFifo,
          RunDecisions& aDecisions,
          OpenLossQuestions aOpenQuestions = OpenLossQuestions::Ask);
};

} // namespace taxbroker
