#include "taxpayer_file.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace taxbroker::cli {

std::variant<TaxpayerMetadata, TaxpayerFileError>
readTaxpayerFile(const std::filesystem::path& aPath) {
    std::error_code error;
    const bool regularFile = std::filesystem::is_regular_file(aPath, error);
    std::ifstream input{aPath, std::ios::binary};

    if (!regularFile || !input)
    {
        return TaxpayerFileError{"The taxpayer file cannot be read."};
    }

    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

    return api::parseTaxpayerJson(text);
}

} // namespace taxbroker::cli
