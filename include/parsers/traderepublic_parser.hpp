#pragma once

#include "parsers/csv_parser.hpp"

namespace taxbroker::tr {

class TradeRepublicParser final : public CsvParser {
  public:
    BrokerStatement parse(const std::filesystem::path& csvPath) override;

  private:
    TradeTransaction parseTradeRow(...);

    DividendTransaction parseDividendRow(...);

    InterestTransaction parseInterestRow(...);

    Date parseDate(...);

    Money parseMoney(...);

    Units parseUnits(...);
};

} // namespace taxbroker::tr
