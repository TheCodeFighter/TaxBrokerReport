#pragma once

#include "processors/tax_processor.hpp"
#include "taxbroker/fifo_basis.hpp"

#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

/// One open purchase lot with its exact remaining units and basis.
struct FifoLot {
    StatementEventReference mPurchase;
    Date mPurchaseDate;
    LotBasisState mState;

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

    bool operator==(const FifoMatch&) const = default;
};

enum class FifoIsinStatus {
    /// Every trade up to 31 December was applied.
    Matched,
    /// Stopped at a corporate action; splits are not applied yet.
    CorporateAction,
    /// Stopped at a trade that is not in EUR; conversion is not applied yet.
    NonEurTrade,
    /// Stopped at a sale that needs more units than the earlier purchases left open. The result
    /// has one incomplete_history error for that sale.
    InsufficientUnits,
    /// Stopped at a value that is invalid or overflows; a diagnostic describes it.
    NumericFailure,
};

struct FifoIsinResult {
    Isin mIsin;
    std::string mName;
    FifoIsinStatus mStatus{FifoIsinStatus::Matched};
    /// The event the pool stopped at; absent when the status is Matched.
    std::optional<StatementEventReference> mStoppedAt;
    /// Open units just before the failing sale; set only for InsufficientUnits.
    std::optional<Units> mAvailableUnits;
    /// Matches of the selected year's sales in ledger order. A stopped pool keeps the matches
    /// made before the stop.
    std::vector<FifoMatch> mMatches;
    /// Matches of the sales before the selected year, in ledger order. Only the 30-day loss rule
    /// reads them: an earlier loss takes its replacement units first.
    std::vector<FifoMatch> mEarlierMatches;
    /// Open lots in FIFO order at 31 December, or just before the event a stopped pool stopped at.
    std::vector<FifoLot> mOpenLots;
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
    /// A pool stops at its first corporate action, non-EUR trade, sale without enough open units
    /// or invalid value, and ignores its later events. The failing sale is never applied
    /// partly: the open lots stay as they were before it. Other ISINs are not affected.
    /// A sale without enough open units gives one incomplete_history error for the capital report,
    /// with the sale's sources and tax date. Only the first such sale of an ISIN is reported.
    ///
    /// aStatement must be the statement that aLedger was prepared from.
    [[nodiscard]] static FifoMatchingResult match(const MergedStatement& aStatement,
                                                  const ReportingYearLedger& aLedger);
};

} // namespace taxbroker
