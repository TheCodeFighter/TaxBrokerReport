#pragma once

#include "processors/fifo_matcher.hpp"
#include "processors/loss_eligibility.hpp"
#include "taxbroker/report_models.hpp"

#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

enum class KdvpBlockReason {
    /// The FIFO pool of the ISIN stopped (corporate action, non-EUR trade, missing history or an
    /// invalid value), so its sales have no reliable matches.
    FifoStopped,
    /// The 30-day loss rule result of a sale is not decided (a question is open or cancelled).
    LossNotReportable,
    /// The unit value (F4) of a purchase row does not fit a UnitPrice, for example after a large
    /// reverse split.
    UnitValueTooLarge,
};

/// An ISIN with a sale in the selected year that cannot get a *popisni list*.
struct KdvpBlockedIsin {
    Isin mIsin;
    std::string mName;
    KdvpBlockReason mReason{KdvpBlockReason::FifoStopped};
    /// The sales of the year that cannot be listed, in ledger order: every sale of the year for
    /// FifoStopped, only the sales whose loss-rule result cannot be written for LossNotReportable,
    /// and only the sales that use a unit value that does not fit for UnitValueTooLarge.
    std::vector<StatementEventReference> mSales;

    bool operator==(const KdvpBlockedIsin&) const = default;
};

/// Units of a sale of the year that are past the holding period (15 years for a sale from 2022,
/// 20 years before). They are tax free and get no row, so they are kept here to stay visible.
struct KdvpLongHeldUnits {
    Isin mIsin;
    std::string mName;
    StatementEventReference mSale;
    /// The units of the sale that are past the holding period and left out of the rows.
    Units mUnits{};

    bool operator==(const KdvpLongHeldUnits&) const = default;
};

struct CapitalGainsBuildResult {
    /// Present only when every blocked ISIN is one the user agreed to leave out or there are no
    /// blocked ISINs.
    std::optional<CapitalGainsReportKDVP> mReport;
    /// Blocked ISINs the user agreed to leave out. They stay visible beside the report and are
    /// never written to the XML.
    std::vector<KdvpBlockedIsin> mExcluded;
    /// Blocked ISINs without the user's agreement. Any entry here means there is no report.
    std::vector<KdvpBlockedIsin> mBlocked;
    /// The units left out of the rows because they are past the holding period, for every ISIN
    /// that is not blocked, by ISIN and then in ledger order.
    std::vector<KdvpLongHeldUnits> mLongHeld;
};

class CapitalGainsReportBuilder final {
  public:
    /// Builds the Doh-KDVP filing rows from the FIFO result.
    [[nodiscard]] static CapitalGainsBuildResult build(const MergedStatement& aStatement,
                                                       const ReportingYearLedger& aLedger,
                                                       const FifoMatchingResult& aFifo,
                                                       const LossEligibilityResult& aLoss,
                                                       const std::vector<Isin>& aExcludedIsins);
};

} // namespace taxbroker
