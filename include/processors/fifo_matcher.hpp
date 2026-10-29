#pragma once

#include "processors/tax_processor.hpp"
#include "taxbroker/fifo_basis.hpp"

#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

// TODO: Adjust this constant to optimal margin days value.
/// A broker may book a corporate action up to this many days after it took effect, or another
/// broker may book the same action this many days apart.
inline constexpr int kBrokerDateMarginDays = 10;

/// One open purchase lot with its exact remaining units and basis.
struct FifoLot {
    StatementEventReference mPurchase;
    Date mPurchaseDate;
    LotBasisState mState;
    bool mChangedByAction{};

    bool operator==(const FifoLot&) const = default;
};

/// Internal link between one sale in the selected year and the units it takes from one purchase.
/// It is not a Doh-KDVP filing row: a sale that uses two purchases has two matches.
struct FifoMatch {
    StatementEventReference mSale;
    StatementEventReference mPurchase;
    Date mSaleDate;
    Date mPurchaseDate;
    Units mUnits{};
    /// Exact purchase basis of the matched units, in ticks of 10^-16 EUR.
    ExactBasis mBasis;
    /// Sale unit price times the matched units, in ticks of 10^-16 EUR. Fees are not included.
    ExactRational mSaleValueTicks;
    /// Sale value minus basis, in ticks of 10^-16 EUR. Negative means a loss-making portion.
    ExactRational mGainTicks;
    bool mLotChangedByAction{};

    bool operator==(const FifoMatch&) const = default;
};

enum class FifoIsinStatus {
    /// Every trade up to 31 December was applied.
    Matched,
    /// Stopped at a corporate action without a confirmed ratio, before the trades of its day.
    CorporateAction,
    /// Stopped at a trade that is not in EUR; conversion is not applied yet.
    NonEurTrade,
    /// Stopped at a sale that needs more units than the earlier purchases left open. The result
    /// has one incomplete_history error for that sale.
    InsufficientUnits,
    /// Stopped at a value that is invalid or overflows, or at a confirmed corporate action that
    /// cannot be applied; a diagnostic describes it.
    NumericFailure,
};

struct ConfirmedCorporateAction {
    /// Every action row of the action, one per broker, all of one ISIN.
    std::vector<StatementEventReference> mEvents;
    SplitRatio mRatio;
    Date mEffectiveDate;

    bool operator==(const ConfirmedCorporateAction&) const = default;
};

struct AppliedCorporateAction {
    StatementEventReference mAction;
    Date mEffectiveDate;

    bool operator==(const AppliedCorporateAction&) const = default;
};

struct FifoIsinResult {
    Isin mIsin;
    std::string mName;
    FifoIsinStatus mStatus{FifoIsinStatus::Matched};
    /// The event the pool stopped at; absent when the status is Matched. For CorporateAction it is
    /// the action, also when a trade of the same day comes first in the ledger. For a confirmed
    /// action that cannot be applied it is the first action row.
    std::optional<StatementEventReference> mStoppedAt;
    /// Open units just before the failing sale; set only for InsufficientUnits.
    std::optional<Units> mAvailableUnits;
    /// For CorporateAction only: a sale before the action that needs more units than were held,
    /// when the action's row is at most kBrokerDateMarginDays after it. The action may have taken
    /// effect before the sale, so the pool stops at the action with the lots just before the
    /// sale, and no incomplete_history error is given.
    std::optional<StatementEventReference> mShortSale;
    /// Matches of the selected year's sales in ledger order. A stopped pool keeps the matches
    /// made before the stop.
    std::vector<FifoMatch> mMatches;
    /// Matches of the sales before the selected year, in ledger order. Only the 30-day loss rule
    /// reads them: an earlier loss takes its replacement units first.
    std::vector<FifoMatch> mEarlierMatches;
    /// Open lots in FIFO order at 31 December, or just before the event a stopped pool stopped at.
    /// For CorporateAction: at the start of the action's day, without the trades of that day.
    std::vector<FifoLot> mOpenLots;
    std::vector<AppliedCorporateAction> mAppliedActions;
};

struct FifoMatchingResult {
    /// One entry for each trade instrument, in the order of the merged presentation (by ISIN).
    std::vector<FifoIsinResult> mIsins;
    std::vector<ProcessingDiagnostic> mDiagnostics;
};

/// Tells whether the match uses units past the holding period, which are tax free.
[[nodiscard]] bool isPastHoldingPeriod(const FifoMatch& aMatch);

class FifoMatcher final {
  public:
    /// Builds one FIFO pool per ISIN from every trade up to 31 December, across all brokers and
    /// files, and matches each sale to the oldest open purchase units. Only sales dated in the
    /// selected year produce matches in mMatches; the matches of earlier sales go to
    /// mEarlierMatches.
    ///
    /// A confirmed corporate action changes every open lot of its ISIN once, at the start of its
    /// effective date, before the trades of that day: the units by the exact ratio; each lot keeps
    /// its purchase date, FIFO place and exact basis. The rounding of the actions of one ISIN must
    /// change its value by less than EUR 0.01 in total; each rounding gives a warning.
    ///
    /// A pool stops at its first corporate action without a confirmed ratio, non-EUR trade, sale
    /// without enough open units, invalid value or confirmed action that cannot be applied, and
    /// ignores its later events. The failing sale is never applied partly: the open lots stay as
    /// they were before it.
    /// A sale without enough open units gives one incomplete_history error for the capital report,
    /// with the sale's sources and tax date. Only the first such sale of an ISIN is reported. When
    /// some units are open, the ISIN has a sale in the selected year and its first unconfirmed
    /// action is a split row at most kBrokerDateMarginDays after the sale, the pool stops at that
    /// action instead (mShortSale), because the split may have taken effect before the sale.
    ///
    /// aStatement must be the statement that aLedger was prepared from.
    [[nodiscard]] static FifoMatchingResult
    match(const MergedStatement& aStatement,
          const ReportingYearLedger& aLedger,
          const std::vector<ConfirmedCorporateAction>& aConfirmedActions = {});
};

} // namespace taxbroker
