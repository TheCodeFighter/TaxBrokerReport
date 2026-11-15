#pragma once

#include "taxbroker/report_models.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace taxbroker::api {

/// Why a taxpayer JSON was refused: one English sentence that names the key. It holds no value.
struct TaxpayerJsonError {
    std::string mMessage; ///< English sentence for the CLI; names the key, never a value.

    bool operator==(const TaxpayerJsonError&) const = default;
};

/// Reads the taxpayer object of the taxpayer file and of the HTTP request: the tax number, the
/// residence and the optional address and contact fields. Unknown keys are refused.
[[nodiscard]] std::variant<TaxpayerMetadata, TaxpayerJsonError>
parseTaxpayerJson(std::string_view aText);

} // namespace taxbroker::api
