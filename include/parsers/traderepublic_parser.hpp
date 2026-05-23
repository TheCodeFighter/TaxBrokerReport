#pragma once

#include "csv_parser.hpp"
#include "types_parser.hpp"

#include <unordered_map>
#include <string_view>

namespace taxbroker::tr {

class TradeRepublicParser final : public CsvParser {
  public:
    ParseResult parse(const std::filesystem::path& csvPath) override;

  private:
    static constexpr char delimiter = ',';

    using Fields = std::vector<std::string>;
    using HeaderMap = std::unordered_map<std::string, std::size_t>;

    const std::string& getField(const Fields& aFields, const HeaderMap& aHeaderMap,
                                const std::string_view& aFieldName) const;

    HeaderMap buildHeaderMap(const Fields& aHeaderFields) const;
    RowType detectRowType(const Fields& aFields, const HeaderMap& aHeaderMap) const;

    TradeTransaction parseTradeRow(const Fields& aFields, const HeaderMap& aHeaderMap);
    DividendTransaction parseDividendRow(const Fields& aFields, const HeaderMap& aHeaderMap);
    InterestTransaction parseInterestRow(const Fields& aFields, const HeaderMap& aHeaderMap);

    Date parseDate(...);

    Money parseMoney(...);

    Units parseUnits(...);
};

} // namespace taxbroker::tr
