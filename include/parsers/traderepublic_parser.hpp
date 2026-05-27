#pragma once

#include "csv_parser.hpp"
#include "types_parser.hpp"

#include <unordered_map>
#include <csv.hpp>

namespace taxbroker::tr {

class TradeRepublicParser final : public CsvParser {
  public:
    ParseResult parse(const std::filesystem::path& aCsvPath) override;

  private:
    static constexpr char delimiter = ',';

    RowType detectRowType(const csv::CSVRow& aCsvRow) const;

    void parseTradeRow(const csv::CSVRow& aCsvRow, std::vector<TradeInstrument>& aInstruments);
    void parseDividendRow(const csv::CSVRow& aCsvRow,
                          std::vector<DividendInstrument>& aInstruments);
    void parseInterestRow(const csv::CSVRow& aCsvRow,
                          std::vector<InterestInstrument>& aInstruments);

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
