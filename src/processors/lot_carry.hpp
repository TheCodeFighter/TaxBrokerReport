#pragma once

#include "processors/statement_merger.hpp"
#include "processors/tax_processor.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"
#include "taxbroker/processing_diagnostics.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

/// Helpers that the ISIN change stage and the stock merger stage share: the rows of one action by
/// ISIN, the ratio the rows show, and moving the lots of the old ISIN to the new one.
namespace taxbroker::lot_carry {

/// The two ISINs of one action and where their rows are, by ISIN (positions move when a change is
/// carried out).
struct IsinChangePair {
    Isin mOldIsin;
    Isin mNewIsin;
    std::vector<std::size_t> mOutRows; ///< Rows of the old instrument that lose units.
    std::vector<std::size_t> mInRows;  ///< Rows of the new instrument that get them.
};

/// Puts trades or action rows in the chronological order of the ledger.
template <typename Event> void sortChronologically(std::vector<Event>& aEvents) {
    std::stable_sort(aEvents.begin(), aEvents.end(), [](const Event& aLeft, const Event& aRight) {
        return ChronologicalEventOrder{}(aLeft.mMetadata, aRight.mMetadata);
    });
}

/// The position of the instrument with this ISIN; the number of instruments when there is none.
[[nodiscard]] std::size_t instrumentIndexOf(const MergedStatement& aStatement, const Isin& aIsin);

/// Tells whether the instrument has a sale in the selected year, or a row of an ISIN change or a
/// stock merger of the year that takes units out (a sale once that change is answered), apart from
/// the rows aOwnRows of the action being checked.
[[nodiscard]] bool hasSaleInYear(const TradeInstrument& aInstrument,
                                 const ReportingYearLedger& aLedger,
                                 const std::vector<std::size_t>& aOwnRows);

/// The total of the units of the rows, positive: the units that left (aLeft) or came in. None when
/// the rows have none on that side or the total does not fit.
[[nodiscard]] std::optional<Units>
totalUnits(const TradeInstrument& aInstrument, const std::vector<std::size_t>& aRows, bool aLeft);

/// The ratio the rows show: the units that came in over the units that left, reduced. None when
/// the rows do not give both totals.
[[nodiscard]] std::optional<SplitRatio>
ratioOfRows(const TradeInstrument& aOld, const TradeInstrument& aNew, const IsinChangePair& aPair);

/// The facts of one action row for a question.
[[nodiscard]] CorporateActionSourceFacts factsOf(const CorporateAction& aAction);

/// The diagnostic of one of the two ISINs of the action, with the sources of every row.
[[nodiscard]] ProcessingDiagnostic isinDiagnostic(DiagnosticSeverity aSeverity,
                                                  ProcessingDiagnosticCode aCode,
                                                  DiagnosticMessage aMessage,
                                                  const TradeInstrument& aInstrument,
                                                  const std::vector<SourceReference>& aSources,
                                                  Date aDate);

/// Tells whether a trade or action row of the old instrument is on or after aDate, or one of the
/// new instrument before it, apart from the rows of the action itself.
[[nodiscard]] bool crossesDate(const TradeInstrument& aOld,
                               const TradeInstrument& aNew,
                               const IsinChangePair& aPair,
                               Date aDate);

/// Moves the lots of the old instrument to the new one and removes the old instrument. The rows of
/// the action become one action row of the new ISIN with the ratio confirmed, or none when the
/// number of units does not change. With aMarkPurchasesAsExchange the moved purchases are marked
/// as received in an exchange at a status change (Doh-KDVP F2 `E`).
void carryOut(MergedStatement& aStatement,
              const IsinChangePair& aPair,
              const IsinChangeAnswer& aAnswer,
              bool aMarkPurchasesAsExchange);

} // namespace taxbroker::lot_carry
