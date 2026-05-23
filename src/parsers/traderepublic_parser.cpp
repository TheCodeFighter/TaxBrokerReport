#include "parsers/traderepublic_parser.hpp"
#include "utils/logger.hpp"

#include <sstream>

namespace taxbroker::tr {
// ParseResult TradeRepublicParser::parse(const std::filesystem::path& csvPath) {}

const std::string& TradeRepublicParser::getField(const Fields& aFields, const HeaderMap& aHeaderMap,
                                                 const std::string_view& aFieldName) const {
    // Might be optimized with transparent hashing if needed
    auto it = aHeaderMap.find(std::string(aFieldName));
    static const std::string empty;

    if (it == aHeaderMap.end()) {
        // warn: missing header
        LOG_ERROR("Missing header: {}", aFieldName);
        return empty;
    }

    if (it->second >= aFields.size()) {
        LOG_WARN("Column missing in this row: {}", aFieldName);
        return empty;
    }

    return aFields[it->second];
}

TradeRepublicParser::HeaderMap
TradeRepublicParser::buildHeaderMap(const Fields& aHeaderFields) const {
    HeaderMap headerMap;

    for (std::size_t i = 0; i < aHeaderFields.size(); ++i) {
        headerMap[aHeaderFields[i]] = i;
    }

    return headerMap;
}

RowType TradeRepublicParser::detectRowType(const Fields& aFields,
                                           const HeaderMap& aHeaderMap) const {
    RowType rowType = RowType::Unknown;

    if (getField(aFields, aHeaderMap, "category") == "TRADING") {
        rowType = RowType::Trade;
    } else if (getField(aFields, aHeaderMap, "category") == "CASH" &&
               getField(aFields, aHeaderMap, "type") == "DIVIDEND") {
        rowType = RowType::Dividend;
    } else if (getField(aFields, aHeaderMap, "category") == "CASH" &&
               getField(aFields, aHeaderMap, "type") == "INTEREST_PAYMENT") {
        rowType = RowType::Interest;
    }

    return rowType;
}

TradeTransaction TradeRepublicParser::parseTradeRow(const Fields& aFields,
                                                    const HeaderMap& aHeaderMap) {
    (void)aFields;
    (void)aHeaderMap;
    return {};
}

} // namespace taxbroker::tr