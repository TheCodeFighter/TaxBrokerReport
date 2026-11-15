#pragma once

#include "taxbroker/api/taxpayer_json.hpp"
#include "taxbroker/report_models.hpp"

#include <filesystem>
#include <variant>

namespace taxbroker::cli {

using TaxpayerFileError = api::TaxpayerJsonError;

/// Reads the taxpayer file at aPath and parses it with `api::parseTaxpayerJson`.
[[nodiscard]] std::variant<TaxpayerMetadata, TaxpayerFileError>
readTaxpayerFile(const std::filesystem::path& aPath);

} // namespace taxbroker::cli
