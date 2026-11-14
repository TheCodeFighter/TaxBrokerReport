#pragma once

#include "taxbroker/report_models.hpp"
#include "xml/xml_write_result.hpp"

namespace taxbroker {

class KdvpXmlWriter final {
  public:
    /// Writes the Doh-KDVP document for the filing rows of the model.
    [[nodiscard]] static XmlWriteResult write(const ReportMetadata& aMetadata,
                                              const CapitalGainsReportKDVP& aReport);
};

} // namespace taxbroker
