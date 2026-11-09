#pragma once

#include "taxbroker/income_report_models.hpp"
#include "taxbroker/report_models.hpp"
#include "xml/xml_write_result.hpp"

namespace taxbroker {

class DivXmlWriter final {
  public:
    /// Writes the Doh-Div document for the rows of the model. The document has no `Dividend` row
    /// for an empty model; the caller decides that there is nothing to file.
    [[nodiscard]] static XmlWriteResult write(const ReportMetadata& aMetadata,
                                              const DividendReportDiv& aReport);
};

} // namespace taxbroker
