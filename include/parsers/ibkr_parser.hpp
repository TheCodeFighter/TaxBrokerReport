#pragma once

#include "csv_parser.hpp"

namespace taxbroker::ibkr {

class IbkrParser final : public CsvParser {
  public:
    ParseResult parse(std::string_view aCsvText,
                      std::string_view aDisplayFilename,
                      std::size_t aSourceIndex) override;

  protected:
    [[nodiscard]] Broker broker() const noexcept override {
        return Broker::InteractiveBrokers;
    }
};

} // namespace taxbroker::ibkr
