#pragma once

#include "processors/capital_gains_report_builder.hpp"
#include "processors/fifo_matcher.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/official_rates.hpp"
#include "taxbroker/reference_catalogue.hpp"

#include <array>

namespace taxbroker {

/// Fills the capital investments and long-held sales of aInput: every ISIN with a sale in the
/// year, by ISIN. aPreview holds the filing rows of the ISINs without a problem.
void appendCapitalSummary(ApplicationResultInput& aInput,
                          const MergedStatement& aStatement,
                          const CapitalGainsBuildResult& aBuilt,
                          const CapitalGainsReportKDVP& aPreview);

/// Fills the applied quantity corrections and, for each ISIN with a rounding impact above 0, its
/// cumulative impact.
void appendCorrections(ApplicationResultInput& aInput,
                       const MergedStatement& aStatement,
                       const FifoMatchingResult& aFifo);

/// Where the reference data and the rates of a run came from. aReferenceOrigins is in the order of
/// ReferenceDataLoad::mOrigins (payers, relief statements, broker payers).
[[nodiscard]] ApplicationDataSources
dataSourcesOf(const std::array<CatalogueOrigin, 3>& aReferenceOrigins,
              const OfficialRateTable& aRates);

} // namespace taxbroker
