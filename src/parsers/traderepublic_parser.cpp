#include "parsers/traderepublic_parser.hpp"
#include "utils/logger.hpp"

#include <sstream>
#include <algorithm>

namespace taxbroker::tr {

ParseResult TradeRepublicParser::parse(const std::filesystem::path& aCsvPath) {
    csv::CSVFormat format;
    format.delimiter(delimiter);
    csv::CSVReader reader(aCsvPath.string(), format);

    ParseResult parsedResult;

    for (csv::CSVRow& row : reader)
    {
        const RowType rowType = detectRowType(row);

        if (rowType == RowType::Unknown)
        {
            continue;
        }

        if (rowType == RowType::Trade)
        {
            parseTradeRow(row, parsedResult.mStatement.mTradeInstruments);
        }
        else if (rowType == RowType::Dividend)
        {
        }
        else if (rowType == RowType::Interest)
        {
        }
    }

    return parsedResult;
}

RowType TradeRepublicParser::detectRowType(const csv::CSVRow& aCsvRow) const {
    RowType rowType = RowType::Unknown;

    if (aCsvRow["category"].get<std::string>() == "TRADING")
    {
        rowType = RowType::Trade;
    }
    else if (aCsvRow["category"].get<std::string>() == "CASH" &&
             aCsvRow["type"].get<std::string>() == "DIVIDEND")
    {
        rowType = RowType::Dividend;
    }
    else if (aCsvRow["category"].get<std::string>() == "CASH" &&
             aCsvRow["type"].get<std::string>() == "INTEREST_PAYMENT")
    {
        rowType = RowType::Interest;
    }

    return rowType;
}

void TradeRepublicParser::parseTradeRow(const csv::CSVRow& aCsvRow,
                                        std::vector<TradeInstrument>& aInstruments) {

    const auto isinValue = aCsvRow["symbol"].get<std::string>();
    const auto nameValue = aCsvRow["name"].get<std::string>();

    if (isinValue.empty() && nameValue.empty())
    {
        LOG_WARNING("Missing ISIN and name values for trade row. Skipping row.");
        return;
    }

    auto logMainFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse row with: {} for {} skipping row", aFieldName, aValue);
    };

    // clang-format off
    if (isinValue.empty()) { logMainFail("ISIN", isinValue); return; }
    if (nameValue.empty()) { logMainFail("name", nameValue); return; }
    // clang-format on

    auto instrumentIt = std::find_if(
        aInstruments.begin(), aInstruments.end(),
        [&](const TradeInstrument& aInstrument) { return aInstrument.mIsin == isinValue; });

    if (instrumentIt == aInstruments.end())
    {
        aInstruments.emplace_back(TradeInstrument{
            .mIsin = isinValue,
            .mName = nameValue,
        });
    }

    if (instrumentIt == aInstruments.end())
    {
        instrumentIt = std::prev(aInstruments.end());
    }

    auto date = parseDate(aCsvRow["date"].get<std::string>());
    auto tradeSide = parseTradeSide(aCsvRow["type"].get<std::string>());
    auto unitPrice = parseUnitPrice(aCsvRow["price"].get<std::string>());
    auto units = parseUnits(aCsvRow["shares"].get<std::string>());
    auto currency = parseCurrency(aCsvRow["currency"].get<std::string>());

    auto logFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse {} value: {} for row with ISIN {}", aFieldName, aValue,
                    isinValue);
    };

    // clang-format off
    if (!date) { logFail("date", aCsvRow["date"].get<std::string>()); return; }
    if (!tradeSide) { logFail("trade side", aCsvRow["type"].get<std::string>()); return; }
    if (!unitPrice) { logFail("unit price", aCsvRow["price"].get<std::string>()); return; }
    if (!units) { logFail("units", aCsvRow["shares"].get<std::string>()); return; }
    if (!currency) { logFail("currency", aCsvRow["currency"].get<std::string>()); return; }
    // clang-format on

    instrumentIt->mTransactions.emplace_back(TradeTransaction{
        .mDate = *date,
        .mTradeSide = *tradeSide,
        .mUnitPrice = *unitPrice,
        .mUnits = *units,
        .mCurrency = *currency,
    });
}

} // namespace taxbroker::tr