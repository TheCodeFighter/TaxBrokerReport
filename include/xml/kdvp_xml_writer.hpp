#pragma once

#include "taxbroker/processing_diagnostics.hpp"
#include "taxbroker/report_models.hpp"

#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

struct XmlWriteResult {
    /// The whole XML document. Present only when there is no error.
    std::optional<std::string> mXml;
    /// One xml_value_out_of_range error for each field that the form does not accept.
    std::vector<ProcessingDiagnostic> mDiagnostics;
};

class KdvpXmlWriter final {
  public:
    /// Writes the Doh-KDVP document for the filing rows of the model.
    [[nodiscard]] static XmlWriteResult write(const ReportMetadata& aMetadata,
                                              const CapitalGainsReportKDVP& aReport);
};

} // namespace taxbroker
