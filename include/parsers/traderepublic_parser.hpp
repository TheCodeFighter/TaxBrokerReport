#pragma once

#include "csv_parser.hpp"
#include "types_parser.hpp"

#include <string_view>

namespace csv {
class CSVRow;
}

namespace taxbroker::tr {

class TradeRepublicParser final : public CsvParser {
  public:
    ParseResult parse(const std::filesystem::path& aCsvPath) override;

  private:
    static constexpr char delimiter = ',';

    RowMeta detectRowType(const csv::CSVRow& aCsvRow) const;
    InterestType detectInterestType(const std::string& aType) const;

    void parseTradeRow(const csv::CSVRow& aCsvRow, std::vector<TradeInstrument>& aInstruments,
                       const RowParsedValues& aParsedValues);
    void parseDividendRow(const csv::CSVRow& aCsvRow,
                          std::vector<DividendInstrument>& aInstruments);
    void parseInterestRow(const csv::CSVRow& aCsvRow, std::vector<InterestInstrument>& aInstruments,
                          const InterestType aInterestType);

    // Parsing helpers
    std::optional<Date> parseDate(std::string_view aValue);
    std::optional<Money> parseMoney(std::string_view aValue);
    std::optional<Units> parseUnits(std::string_view aValue);
    std::optional<Currency> parseCurrency(std::string_view aValue);
    std::optional<TradeSide> parseTradeSide(std::string_view aValue);

    // class helpers
    static bool isInstrumentValid(std::string_view aContext, const std::string& aIsin,
                                  const std::string& aName);
};

} // namespace taxbroker::tr
