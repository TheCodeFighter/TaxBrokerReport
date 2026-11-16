#pragma once

#include "taxbroker/report_models.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace taxbroker::api {

/// Why a taxpayer JSON was refused: one English sentence that names the key. It holds no value.
struct TaxpayerJsonError {
    std::string mMessage; ///< English sentence for the CLI; names the key, never a value.
    /// The fixed taxpayer key the error is about (`taxNumber`, `birthDate`, ...) for the browser
    /// form. Empty when the text is not JSON or not an object, and for a key the application does
    /// not know (the name is the sender's text, so it is never repeated).
    std::string mKey;

    bool operator==(const TaxpayerJsonError&) const = default;
};

/// Reads the taxpayer object of the taxpayer file and of the HTTP request: the tax number, the
/// residence and the optional address and contact fields. Unknown keys are refused.
[[nodiscard]] std::variant<TaxpayerMetadata, TaxpayerJsonError>
parseTaxpayerJson(std::string_view aText);

} // namespace taxbroker::api
