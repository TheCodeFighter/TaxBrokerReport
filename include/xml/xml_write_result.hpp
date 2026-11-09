#pragma once

#include "taxbroker/processing_diagnostics.hpp"

#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

/// What an XML writer returns: the document, or the errors that stopped it.
struct XmlWriteResult {
    /// The whole XML document. Present only when there is no error.
    std::optional<std::string> mXml;
    /// One xml_value_out_of_range error for each field that the form does not accept.
    std::vector<ProcessingDiagnostic> mDiagnostics;
};

} // namespace taxbroker
