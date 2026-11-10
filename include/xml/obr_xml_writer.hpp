#pragma once

#include "taxbroker/income_report_models.hpp"
#include "taxbroker/report_models.hpp"
#include "xml/xml_write_result.hpp"

namespace taxbroker {

class ObrXmlWriter final {
  public:
    /// Writes the Doh-Obr document for the rows of the model: broker cash interest and bond
    /// coupons together. The document has no `Interest` row for an empty model; the caller decides
    /// that there is nothing to file.
    [[nodiscard]] static XmlWriteResult write(const ReportMetadata& aMetadata,
                                              const OtherInterestReportObr& aReport);
};

} // namespace taxbroker
