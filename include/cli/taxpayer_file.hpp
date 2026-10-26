#pragma once

#include "taxbroker/report_models.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <variant>

namespace taxbroker::cli {

struct TaxpayerFileError {
    std::string mMessage;

    bool operator==(const TaxpayerFileError&) const = default;
};

[[nodiscard]] std::variant<TaxpayerMetadata, TaxpayerFileError>
parseTaxpayerJson(std::string_view aText);

[[nodiscard]] std::variant<TaxpayerMetadata, TaxpayerFileError>
readTaxpayerFile(const std::filesystem::path& aPath);

} // namespace taxbroker::cli
