#pragma once

#include "processors/fifo_matcher.hpp"
#include "processors/loss_eligibility.hpp"
#include "taxbroker/diagnostic_messages.hpp"
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

/// An ISIN with a sale in the selected year that the user left out of the report. It gets no
/// rows, whether or not it was blocked.
struct KdvpExcludedIsin {
    Isin mIsin;
    std::string mName;
    /// Why the builder blocked it, when it did.
    std::optional<KdvpBlockReason> mBlockReason;
    /// Every sale of the year, in ledger order.
    std::vector<StatementEventReference> mSales;

    bool operator==(const KdvpExcludedIsin&) const = default;
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

/// A purchase row whose exact basis differs from its F4 times its F3 once both are rounded to
/// four decimals. Output only: the exact basis stays in the FIFO lot and nothing is repaired.
struct KdvpBasisDifference {
    Isin mIsin;
    std::string mName;
    StatementEventReference mPurchase; ///< The purchase of the row.

    bool operator==(const KdvpBasisDifference&) const = default;
};

struct CapitalGainsBuildResult {
    /// Present only when every blocked ISIN is one the user agreed to leave out or there are no
    /// blocked ISINs.
    std::optional<CapitalGainsReportKDVP> mReport;
    /// ISINs with a sale of the year that the user agreed to leave out, blocked or not. They stay
    /// visible beside the report and are never written to the XML.
    std::vector<KdvpExcludedIsin> mExcluded;
    /// Blocked ISINs without the user's agreement. Any entry here means there is no report.
    std::vector<KdvpBlockedIsin> mBlocked;
    /// The units left out of the rows because they are past the holding period, for every ISIN
    /// that is not blocked, by ISIN and then in ledger order.
    std::vector<KdvpLongHeldUnits> mLongHeld;
    /// The purchase rows of mReport's lists with a basis difference, by ISIN and row order. Empty
    /// when there is no report.
    std::vector<KdvpBasisDifference> mBasisDifferences;
};

/// One xml_basis_rounding_difference warning for each entry of aBuilt.mBasisDifferences. No
/// units or amounts in the message.
[[nodiscard]] std::vector<ProcessingDiagnostic>
basisDifferenceDiagnostics(const MergedStatement& aStatement,
                           const CapitalGainsBuildResult& aBuilt);

class CapitalGainsReportBuilder final {
  public:
    /// Builds the Doh-KDVP filing rows from the FIFO result, without the ISINs in aExcludedIsins.
    [[nodiscard]] static CapitalGainsBuildResult build(const MergedStatement& aStatement,
                                                       const ReportingYearLedger& aLedger,
                                                       const FifoMatchingResult& aFifo,
                                                       const LossEligibilityResult& aLoss,
                                                       const std::vector<Isin>& aExcludedIsins);
};

} // namespace taxbroker
