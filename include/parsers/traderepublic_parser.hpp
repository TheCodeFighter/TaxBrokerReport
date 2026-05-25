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
    DividendTransaction parseDividendRow(const csv::CSVRow& aCsvRow,
                                         std::vector<DividendInstrument>& aInstruments);
    InterestTransaction parseInterestRow(const csv::CSVRow& aCsvRow,
                                         std::vector<InterestInstrument>& aInstruments);

    std::optional<Date> parseDate(std::string_view aValue);
    std::optional<Money> parseUnitPrice(std::string_view aValue);
    std::optional<Units> parseUnits(std::string_view aValue);
    std::optional<Currency> parseCurrency(std::string_view aValue);
    std::optional<TradeSide> parseTradeSide(std::string_view aValue);
};

} // namespace taxbroker::tr
