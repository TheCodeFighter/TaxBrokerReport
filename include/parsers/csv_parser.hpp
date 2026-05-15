#pragma once

#include <filesystem>

#include "taxbroker/types.hpp"

namespace taxbroker {

class CsvParser {
  public:
    virtual ~CsvParser() = default;

    [[nodiscard("Parsed broker data should not be ignored")]] virtual BrokerStatement
    parse(const std::filesystem::path& csvPath) = 0;
};

} // namespace taxbroker
