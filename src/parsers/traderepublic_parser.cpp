#include "parsers/traderepublic_parser.hpp"
#include "utils/logger.hpp"

#include <sstream>
#include <algorithm>
#include <csv.hpp>
#include <string_view>

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
            .mName = aName,
            .mIsin = aIsin,
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
        const RowMeta rowMeta = detectRowType(row);

        if (rowMeta.mRowType == RowType::Unknown)
        {
            continue;
        }

        if (rowMeta.mRowType == RowType::Trade)
        {
            parseTradeRow(row, parsedResult.mStatement.mTradeInstruments, rowMeta.mParsedValues);
        }
        else if (rowMeta.mRowType == RowType::Dividend)
        {
            parseDividendRow(row, parsedResult.mStatement.mDividendInstruments);
        }
        else if (rowMeta.mRowType == RowType::Cash)
        {
            const auto interestType = detectInterestType(rowMeta.mParsedValues.mType);

            // other transactions with cash category (ie CARD_TRANSACTION) are skipped, not relevant
            // for tax report
            if (interestType != InterestType::UnknownInterest)
            {
                parseInterestRow(row, parsedResult.mStatement.mInterestInstruments, interestType);
            }
        }
    }

    return parsedResult;
}

RowMeta TradeRepublicParser::detectRowType(const csv::CSVRow& aCsvRow) const {
    RowType rowType = RowType::Unknown;

    auto category = aCsvRow["category"].get<std::string>();
    auto type = aCsvRow["type"].get<std::string>();

    if (category == "TRADING")
    {
        rowType = RowType::Trade;
    }
    else if (category == "CASH" && type == "DIVIDEND")
    {
        rowType = RowType::Dividend;
    }
    else if (category == "CASH" && type == "INTEREST_PAYMENT")
    {
        rowType = RowType::Cash;
    }

    return RowMeta{
        .mRowType = rowType,
        .mParsedValues =
            RowParsedValues{
                .mCategory = std::move(category),
                .mType = std::move(type),
            },
    };
}

InterestType TradeRepublicParser::detectInterestType(const std::string& aType) const {
    if (aType == "INTEREST_PAYMENT")
    {
        return InterestType::BrokerInterest;
    } // TODO: or fix income or bond type
    else if (aType == "BOND_INTEREST")
    {
        return InterestType::BondInterest;
    }
    else if (aType == "FIXED_INCOME")
    {
        return InterestType::OtherInterest;
    }
    else
    {
        return InterestType::UnknownInterest;
    }
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
                                        std::vector<TradeInstrument>& aInstruments,
                                        const RowParsedValues& aParsedValues) {

    const auto isinValue = aCsvRow["symbol"].get<std::string>();
    const auto nameValue = aCsvRow["name"].get<std::string>();
    std::string_view typeValue = aParsedValues.mType;

    if (!isInstrumentValid("trade", isinValue, nameValue))
    {
        return;
    }

    auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);

    auto date = parseDate(aCsvRow["date"].get<std::string>());
    auto tradeSide = parseTradeSide(typeValue);
    auto unitPrice = parseMoney(aCsvRow["price"].get<std::string>());
    auto units = parseUnits(aCsvRow["shares"].get<std::string>());
    auto currency = parseCurrency(aCsvRow["currency"].get<std::string>());

    auto logFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse {} value: {} for row with ISIN {}", aFieldName, aValue,
                    isinValue);
    };

    // clang-format off
    if (!date) { logFail("date", aCsvRow["date"].get<std::string>()); return; }
    if (!tradeSide) { logFail("trade side", typeValue); return; }
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
    auto grossAmount = parseMoney(aCsvRow["amount"].get<std::string>());
    auto taxPaid = parseMoney(aCsvRow["tax"].get<std::string>());
    auto currency = parseCurrency(aCsvRow["currency"].get<std::string>());

    auto logFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse {} value: {} for row with ISIN {}", aFieldName, aValue,
                    isinValue);
    };

    // clang-format off
    if (!date) { logFail("date", aCsvRow["date"].get<std::string>()); return; }
    if (!grossAmount) { logFail("amount", aCsvRow["amount"].get<std::string>()); return; }
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

void TradeRepublicParser::parseInterestRow(const csv::CSVRow& aCsvRow,
                                           std::vector<InterestInstrument>& aInstruments,
                                           const InterestType aInterestType) {

    const auto nameValue = aCsvRow["name"].get<std::string>();
    std::string isinValue{};

    if (aInterestType == InterestType::BondInterest)
    {
        isinValue = aCsvRow["symbol"].get<std::string>();

        if (!isInstrumentValid("interest", isinValue, nameValue))
        {
            return;
        }

        auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);
        (void)instrument;
    }
    // Broker interest and other interest types don't have ISIN
    else
    {
        if (nameValue.empty())
        {
            LOG_WARNING("Missing name value for interest row. Skipping row.");
            return;
        }
    }
}

} // namespace taxbroker::tr