#pragma once

#include "taxbroker/statement_merger.hpp"
#include "taxbroker/types.hpp"

#include <string>
#include <vector>

namespace taxbroker {

/// One dividend made from the cash rows of one payment.
struct NettedDividend {
    std::string mName; ///< The rows' instrument name; empty when no row names the instrument.
    Isin mIsin;
    DividendTransaction mEvent;
};

struct IncomePaymentNettingResult {
    std::vector<NettedDividend> mDividends;
    std::vector<MergeDiagnostic> mDiagnostics;
};

/// Adds up the cash rows of each payment (one broker, one payment group ID, one ISIN) into one
/// dividend: gross = dividends - reversals, tax = taxes - refunds. A payment whose rows cannot be
/// added up, has no dividend row, or nets to 0 or less gives a diagnostic and no dividend.
[[nodiscard]] IncomePaymentNettingResult
netIncomePayments(const std::vector<IncomePaymentRow>& aRows);

} // namespace taxbroker
