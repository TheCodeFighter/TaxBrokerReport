#pragma once

#include "taxbroker/application_result.hpp"
#include "processors/tax_processor.hpp"
#include "taxbroker/statement_merger.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

/// A bond's purchases, sales and maturities in the reporting year.
struct BondYear {
    std::string mName;
    std::vector<SourceReference> mSources; ///< The rows of all these events.
    std::optional<Date> mFirstDate;        ///< The date of the first of them.
    BondExclusionFacts mUnits;
    bool mOverflow{}; ///< A unit total did not fit; mUnits is not exact then.
};

/**
 * The bonds with an event in the reporting year, by ISIN: the purchases and sales of the trade
 * instruments of class Bond and the maturity rows. The units of a maturity are added up by sum
 * (units out minus units back), never by order.
 */
[[nodiscard]] std::map<Isin, BondYear> bondYearsOf(const MergedStatement& aStatement,
                                                   const ReportingYearLedger& aLedger);

} // namespace taxbroker
