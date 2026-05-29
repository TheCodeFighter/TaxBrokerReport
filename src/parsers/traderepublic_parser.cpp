#include "parsers/traderepublic_parser.hpp"
#include "utils/logger.hpp"

#include <sstream>
#include <algorithm>
#include <csv.hpp>

namespace {
// used just for trade rows and dividend rows
template <typename InstrumentT>
InstrumentT& getOrCreateInstrument(std::vector<InstrumentT>& aInstruments, const std::string& aIsin,
                                   const std::string& aName) {
    auto instrumentIt =
        std::find_if(aInstruments.begin(), aInstruments.end(),
                     [&](const InstrumentT& aInstrument) { return aInstrument.mIsin == aIsin; });

    if (instrumentIt == aInstruments.end())
    {
        aInstruments.emplace_back(InstrumentT{
            .mIsin = aIsin,
            .mName = aName,
        });
        instrumentIt = std::prev(aInstruments.end());
    }

    return *instrumentIt;
}

} // namespace

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

bool TradeRepublicParser::isInstrumentValid(std::string_view aContext, const std::string& aIsin,
                                            const std::string& aName) {
    if (aIsin.empty() && aName.empty())
    {
        LOG_WARNING("Missing ISIN and name values for {} row. Skipping row.", aContext);
        return false;
    }

    auto logMainFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse {} row with: {} for {} skipping row", aContext, aFieldName,
                    aValue);
    };

    // clang-format off
    if (aIsin.empty()) { logMainFail("ISIN", aIsin); return false; }
    if (aName.empty()) { logMainFail("name", aName); return false; }
    // clang-format on
    return true;
}

void TradeRepublicParser::parseTradeRow(const csv::CSVRow& aCsvRow,
                                        std::vector<TradeInstrument>& aInstruments) {

    const auto isinValue = aCsvRow["symbol"].get<std::string>();
    const auto nameValue = aCsvRow["name"].get<std::string>();

    if (!isInstrumentValid("trade", isinValue, nameValue))
    {
        return;
    }

    auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);

    auto date = parseDate(aCsvRow["date"].get<std::string>());
    auto tradeSide = parseTradeSide(aCsvRow["type"].get<std::string>());
    auto unitPrice = parseMoney(aCsvRow["price"].get<std::string>());
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

    instrument.mTransactions.emplace_back(TradeTransaction{
        .mDate = *date,
        .mTradeSide = *tradeSide,
        .mUnitPrice = *unitPrice,
        .mUnits = *units,
        .mCurrency = *currency,
    });
}

void TradeRepublicParser::parseDividendRow(const csv::CSVRow& aCsvRow,
                                           std::vector<DividendInstrument>& aInstruments) {
    const auto isinValue = aCsvRow["symbol"].get<std::string>();
    const auto nameValue = aCsvRow["name"].get<std::string>();

    if (!isInstrumentValid("dividend", isinValue, nameValue))
    {
        return;
    }

    auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);

    auto date = parseDate(aCsvRow["date"].get<std::string>());
    auto grossAmount = parseMoney(aCsvRow["gross amount"].get<std::string>());
    auto taxPaid = parseMoney(aCsvRow["tax"].get<std::string>());
    auto currency = parseCurrency(aCsvRow["currency"].get<std::string>());

    auto logFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse {} value: {} for row with ISIN {}", aFieldName, aValue,
                    isinValue);
    };

    // clang-format off
    if (!date) { logFail("date", aCsvRow["date"].get<std::string>()); return; }
    if (!grossAmount) { logFail("gross amount", aCsvRow["amount"].get<std::string>()); return; }
    if (!taxPaid) { logFail("tax paid", aCsvRow["tax"].get<std::string>()); return; }
    if (!currency) { logFail("currency", aCsvRow["currency"].get<std::string>()); return; }
    // clang-format on

    instrument.mTransactions.emplace_back(DividendTransaction{
        .mDate = *date,
        .mGrossAmount = *grossAmount,
        .mTaxPaid = *taxPaid,
        .mCurrency = *currency,
    });
}

} // namespace taxbroker::tr