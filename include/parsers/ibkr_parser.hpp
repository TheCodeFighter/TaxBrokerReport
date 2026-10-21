#pragma once

#include "csv_parser.hpp"

namespace taxbroker::ibkr {

class IbkrParser final : public CsvParser {
  public:
    ParseResult parse(const std::filesystem::path& aCsvPath, std::size_t aSourceIndex = 0) override;
};

} // namespace taxbroker::ibkr
