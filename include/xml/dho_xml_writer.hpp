#pragma once

#include "taxbroker/income_report_models.hpp"
#include "taxbroker/report_models.hpp"
#include "xml/xml_write_result.hpp"

namespace taxbroker {

class DhoXmlWriter final {
  public:
    /// Writes the Doh-DHO document for the rows of the model and their totals. The document has no
    /// `Doh_DHO_InterestEarned` row for an empty model; the caller decides that there is nothing
    /// to file.
    [[nodiscard]] static XmlWriteResult write(const ReportMetadata& aMetadata,
                                              const DepositInterestReportDHO& aReport);
};

} // namespace taxbroker
