#pragma once

#include "csv_parser.hpp"

namespace taxbroker::ibkr {

class IbkrParser final : public CsvParser {
  public:
    ParseResult parse(const std::filesystem::path& csvPath) override;

  private:
    TradeTransaction parseTradeRow(...);

    DividendTransaction parseDividendRow(...);

    InterestTransaction parseInterestRow(...);

    Date parseDate(...);

    Money parseMoney(...);

    Units parseUnits(...);
};

} // namespace taxbroker::ibkr
