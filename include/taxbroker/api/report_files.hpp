#pragma once

#include "taxbroker/processing_diagnostics.hpp"

#include <string>
#include <string_view>

namespace taxbroker::api {

/// The FURS form code of a report: `Doh-KDVP`, `Doh-Div`, `Doh-DHO` or `Doh-Obr`.
[[nodiscard]] std::string_view reportFormCode(ReportType aType);

/// The file name of a generated XML.
[[nodiscard]] std::string reportXmlFileName(ReportType aType, int aYear, bool aUnsafe);

} // namespace taxbroker::api
