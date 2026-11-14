#pragma once

#include "processors/official_rate_selection.hpp"
#include "processors/tax_processor.hpp"

#include <optional>
#include <vector>

namespace taxbroker {

/// One foreign trade whose unit price was converted to EUR.
struct ConvertedTrade {
    StatementEventReference mTrade;
    Currency mOriginalCurrency{Currency::Unknown};
    UnitPrice mOriginalUnitPrice{};
    UnitPrice mEurUnitPrice{};
    OfficialRate mRate; ///< The rate used; its own date can be earlier than the trade's tax date.
};

/// The rates of two lists by date, then currency code, one per currency and day.
[[nodiscard]] std::vector<UsedOfficialRate> mergedRates(std::vector<UsedOfficialRate> aFirstRates,
                                                        std::vector<UsedOfficialRate> aSecondRates);

/// What convertCapitalTrades did: the converted copy, each conversion and the rates it used.
struct TradeConversionResult {
    /// The statement with the converted trades in EUR; absent when no trade was converted.
    std::optional<MergedStatement> mConvertedStatement;
    std::vector<ConvertedTrade> mConverted;
    std::vector<UsedOfficialRate> mUsedRates; ///< One per currency and day, by date, then code.
    OfficialRateSelection mRates; ///< The selection, with the error of each pair without a rate.
    std::vector<ProcessingDiagnostic> mDiagnostics; ///< Rate warnings and price conversion errors.

    /// The statement the capital stages must use: the converted one, or aOriginal.
    [[nodiscard]] const MergedStatement& statement(const MergedStatement& aOriginal) const {
        return mConvertedStatement ? *mConvertedStatement : aOriginal;
    }
};

/**
 * Converts the unit price of every foreign trade dated up to the end of the reporting year to EUR,
 * at the official rate of the trade's own tax date, rounded once to eight decimals. Trades of an
 * ISIN that is left out, trades after the year and trades in EUR or in an unknown currency are not
 * changed. A trade without a rate or whose price cannot be converted stays foreign, so the FIFO
 * pool of its ISIN stops there; the reason is in mRates (no rate) or mDiagnostics (price).
 */
[[nodiscard]] TradeConversionResult convertCapitalTrades(const MergedStatement& aStatement,
                                                         const ReportingYearLedger& aLedger,
                                                         const OfficialRateTable& aTable);

} // namespace taxbroker
