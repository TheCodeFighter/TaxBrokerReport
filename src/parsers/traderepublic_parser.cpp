#include "parsers/traderepublic_parser.hpp"
#include "taxbroker/types.hpp"
#include "utils/logger.hpp"
#include "utils/numeric_util.hpp"

#include <algorithm>
#include <charconv>
#include <csv.hpp>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <system_error>

namespace {
// used just for trade rows and dividend rows
template <typename InstrumentT>
InstrumentT& getOrCreateInstrument(std::vector<InstrumentT>& aInstruments,
                                   const std::string& aIsin,
                                   const std::string& aName) {
    auto instrumentIt =
        std::find_if(aInstruments.begin(), aInstruments.end(), [&](const InstrumentT& aInstrument) {
            return aInstrument.mIsin == aIsin;
        });

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
        else if (rowMeta.mRowType == RowType::Interest)
        {
            const auto interestType = detectInterestType(rowMeta.mParsedValues.mType);
            parseInterestRow(row, parsedResult.mStatement.mInterestInstruments, interestType);
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
    else if (category == "CASH" &&
             (type == "INTEREST_PAYMENT" || type == "BOND_INTEREST" || type == "FIXED_INCOME"))
    {
        rowType = RowType::Interest;
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

bool TradeRepublicParser::isInstrumentValid(std::string_view aContext,
                                            const std::string& aIsin,
                                            const std::string& aName) {
    if (aIsin.empty() && aName.empty())
    {
        LOG_WARNING("Missing ISIN and name values for {} row. Skipping row.", aContext);
        return false;
    }

    auto logMainFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse {} row with: {} for {} skipping row",
                    aContext,
                    aFieldName,
                    aValue);
    };

    if (aIsin.empty())
    {
        logMainFail("ISIN", aIsin);
        return false;
    }

    if (aName.empty())
    {
        logMainFail("name", aName);
        return false;
    }

    return true;
}

GetAmount TradeRepublicParser::getAmountAndCurrency(const csv::CSVRow& aCsvRow) {
    std::optional<Money> grossAmount{};
    std::optional<Money> exchangeRate{MONEY_SCALE}; // Default to 1.0 (scaled)

    auto currency = parseCurrency(aCsvRow["original_currency"].get<std::string>());
    if (currency == Currency::Unknown)
    {
        currency = parseCurrency(aCsvRow["currency"].get<std::string>());
    }

    if (currency == Currency::Unknown)
    {
        LOG_WARNING("Unknown currency. Skipping row for {}", aCsvRow["name"].get<std::string>());
        exchangeRate.reset();
        return GetAmount{
            .mGrossAmount = grossAmount,
            .mExchangeRate = exchangeRate,
            .mCurrency = std::nullopt,
        };
    }
    else if (currency == Currency::EUR)
    {
        grossAmount = parseMoney(aCsvRow["amount"].get<std::string>());
    }
    else
    {
        grossAmount = parseMoney(aCsvRow["original_amount"].get<std::string>());
        exchangeRate = parseMoney(aCsvRow["fx_rate"].get<std::string>());
    }

    return GetAmount{
        .mGrossAmount = grossAmount,
        .mExchangeRate = exchangeRate,
        .mCurrency = currency,
    };
}

std::pair<std::string_view, std::string>
TradeRepublicParser::pickAmountField(const csv::CSVRow& aRow) {
    const auto originalAmount = aRow["original_amount"].get<std::string>();
    auto effectiveCurrency = parseCurrency(aRow["original_currency"].get<std::string>());
    if (effectiveCurrency == Currency::Unknown)
    {
        effectiveCurrency = parseCurrency(aRow["currency"].get<std::string>());
    }

    if (effectiveCurrency != Currency::EUR && effectiveCurrency != Currency::Unknown)
    {
        return {"original_amount", originalAmount};
    }

    return {"amount", aRow["amount"].get<std::string>()};
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
    // Warning, if currency is null, we have a problem
    auto currency = parseCurrency(aCsvRow["currency"].get<std::string>());

    auto logFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse {} value: {} for row with ISIN {}",
                    aFieldName,
                    aValue,
                    isinValue);
    };

    if (!date)
    {
        logFail("date", aCsvRow["date"].get<std::string>());
        return;
    }

    if (!tradeSide)
    {
        logFail("trade side", typeValue);
        return;
    }

    if (!unitPrice)
    {
        logFail("unit price", aCsvRow["price"].get<std::string>());
        return;
    }

    if (!units)
    {
        logFail("units", aCsvRow["shares"].get<std::string>());
        return;
    }

    const auto normalizedUnits = normalizeTradeUnits(*tradeSide, *units);
    if (!normalizedUnits)
    {
        logFail("units inconsistent with trade side", aCsvRow["shares"].get<std::string>());
        return;
    }

    if (currency == Currency::Unknown)
    {
        logFail("currency", aCsvRow["currency"].get<std::string>());
        return;
    }

    instrument.mTransactions.emplace_back(TradeTransaction{
        .mDate = *date,
        .mTradeSide = *tradeSide,
        .mUnitPrice = *unitPrice,
        .mUnits = *normalizedUnits,
        .mExchangeRate = MONEY_SCALE, // Default to 1.0 (scaled)
        .mCurrency = currency,
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
    auto taxPaid = parseTaxPaid(aCsvRow["tax"].get<std::string>());
    auto amountAndCurrency = getAmountAndCurrency(aCsvRow);

    auto logFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse {} value: {} for row with ISIN {}",
                    aFieldName,
                    aValue,
                    isinValue);
    };

    if (!date)
    {
        logFail("date", aCsvRow["date"].get<std::string>());
        return;
    }

    if (!taxPaid)
    {
        logFail("tax paid", aCsvRow["tax"].get<std::string>());
        return;
    }

    if (!amountAndCurrency.mCurrency.has_value())
    {
        logFail("currency", aCsvRow["currency"].get<std::string>());
        return;
    }

    if (!amountAndCurrency.mExchangeRate.has_value())
    {
        logFail("fx rate", aCsvRow["fx_rate"].get<std::string>());
        return;
    }

    if (!amountAndCurrency.mGrossAmount.has_value())
    {
        const auto [fieldName, fieldValue] = pickAmountField(aCsvRow);
        logFail(fieldName, fieldValue);
        return;
    }

    instrument.mTransactions.emplace_back(DividendTransaction{
        .mDate = *date,
        .mGrossAmount = *amountAndCurrency.mGrossAmount,
        .mTaxPaid = *taxPaid,
        .mExchangeRate = *amountAndCurrency.mExchangeRate,
        .mCurrency = *amountAndCurrency.mCurrency,
    });
}

void TradeRepublicParser::parseInterestRow(const csv::CSVRow& aCsvRow,
                                           std::vector<InterestInstrument>& aInstruments,
                                           const InterestType aInterestType) {
    if (aInterestType == InterestType::UnknownInterest)
    {
        LOG_WARNING("Unknown interest type for row with name {}. Skipping row.",
                    aCsvRow["name"].get<std::string>());
        return;
    }

    if (aInterestType == InterestType::BondInterest)
    {
        const auto nameValue = aCsvRow["name"].get<std::string>();
        std::string isinValue = aCsvRow["symbol"].get<std::string>();

        if (!isInstrumentValid("interest", isinValue, nameValue))
        {
            return;
        }

        auto date = parseDate(aCsvRow["date"].get<std::string>());
        auto taxPaid = parseTaxPaid(aCsvRow["tax"].get<std::string>());

        auto amountAndCurrency = getAmountAndCurrency(aCsvRow);

        auto logFail = [&](std::string_view aFieldName, std::string_view aValue) {
            LOG_WARNING("Failed to parse {} value: {} for interest row with name {}",
                        aFieldName,
                        aValue,
                        nameValue);
        };

        if (!date)
        {
            logFail("date", aCsvRow["date"].get<std::string>());
            return;
        }

        if (!taxPaid)
        {
            logFail("tax paid", aCsvRow["tax"].get<std::string>());
            return;
        }

        if (!amountAndCurrency.mCurrency.has_value())
        {
            logFail("currency", aCsvRow["currency"].get<std::string>());
            return;
        }

        if (!amountAndCurrency.mExchangeRate.has_value())
        {
            logFail("fx rate", aCsvRow["fx_rate"].get<std::string>());
            return;
        }

        if (!amountAndCurrency.mGrossAmount.has_value())
        {
            const auto [fieldName, fieldValue] = pickAmountField(aCsvRow);
            logFail(fieldName, fieldValue);
            return;
        }

        auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);

        instrument.mInterestType = InterestType::BondInterest;

        instrument.mTransactions.emplace_back(InterestTransaction{
            .mDate = *date,
            .mGrossAmount = *amountAndCurrency.mGrossAmount,
            .mTaxPaid = *taxPaid,
            .mExchangeRate = *amountAndCurrency.mExchangeRate,
            .mCurrency = *amountAndCurrency.mCurrency,
        });
    }
    // Broker interest and other interest types don't have ISIN
    else
    {
        // Currently we don't have info how fixed income interest looks like and will be skipped for
        // now
        if (aInterestType == InterestType::OtherInterest)
        {
            LOG_WARNING(
                "Skipping fixed income interest row with name {} as it's currently not supported.",
                aCsvRow["name"].get<std::string>());
            return;
        }

        if (aInterestType == InterestType::BrokerInterest)
        {
            const auto brokerName = "Trade Republic";
            auto instrumentIt = std::find_if(aInstruments.begin(),
                                             aInstruments.end(),
                                             [&](const InterestInstrument& aInstrument) {
                                                 return aInstrument.mName == brokerName;
                                             });

            if (instrumentIt == aInstruments.end())
            {
                aInstruments.emplace_back(
                    InterestInstrument{.mName = brokerName,
                                       .mInterestType = InterestType::BrokerInterest});
                instrumentIt = std::prev(aInstruments.end());
            }

            auto date = parseDate(aCsvRow["date"].get<std::string>());
            auto taxPaid = parseTaxPaid(aCsvRow["tax"].get<std::string>());

            auto amountAndCurrency = getAmountAndCurrency(aCsvRow);

            auto logFail = [&](std::string_view aFieldName, std::string_view aValue) {
                LOG_WARNING("Failed to parse {} value: {} for broker interest row. Skipping row.",
                            aFieldName,
                            aValue);
            };

            if (!date)
            {
                logFail("date", aCsvRow["date"].get<std::string>());
                return;
            }

            if (!taxPaid)
            {
                logFail("tax paid", aCsvRow["tax"].get<std::string>());
                return;
            }

            if (!amountAndCurrency.mCurrency.has_value())
            {
                logFail("currency", aCsvRow["currency"].get<std::string>());
                return;
            }

            if (!amountAndCurrency.mExchangeRate.has_value())
            {
                logFail("fx rate", aCsvRow["fx_rate"].get<std::string>());
                return;
            }

            if (!amountAndCurrency.mGrossAmount)
            {
                const auto [fieldName, fieldValue] = pickAmountField(aCsvRow);
                logFail(fieldName, fieldValue);
                return;
            }

            instrumentIt->mTransactions.emplace_back(
                InterestTransaction{.mDate = *date,
                                    .mGrossAmount = *amountAndCurrency.mGrossAmount,
                                    .mTaxPaid = *taxPaid,
                                    .mExchangeRate = *amountAndCurrency.mExchangeRate,
                                    .mCurrency = *amountAndCurrency.mCurrency});
        }

        // Broker interest
    }
}

std::optional<Date> TradeRepublicParser::parseDate(std::string_view aValue) {
    if (aValue.size() != 10 || aValue[4] != '-' || aValue[7] != '-')
        return std::nullopt;

    int year{}, month{}, day{};
    auto view_year = aValue.substr(0, 4);
    auto view_month = aValue.substr(5, 2);
    auto view_day = aValue.substr(8, 2);

    if (std::from_chars(view_year.data(), view_year.data() + view_year.size(), year).ec !=
            std::errc{} ||
        std::from_chars(view_month.data(), view_month.data() + view_month.size(), month).ec !=
            std::errc{} ||
        std::from_chars(view_day.data(), view_day.data() + view_day.size(), day).ec != std::errc{})
    {
        return std::nullopt;
    }

    auto ymd = std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} /
               std::chrono::day{static_cast<unsigned>(day)};
    if (!ymd.ok())
        return std::nullopt;

    return Date{std::chrono::time_point_cast<DayDuration>(std::chrono::sys_days{ymd})};
}

std::optional<Money> TradeRepublicParser::parseMoney(std::string_view aValue) {
    return parseScaledNumber<Money, MONEY_SCALE>(aValue);
}

std::optional<Units> TradeRepublicParser::parseUnits(std::string_view aValue) {
    return parseScaledNumber<Units, UNITS_SCALE>(aValue);
}

std::optional<Units> TradeRepublicParser::normalizeTradeUnits(TradeSide aTradeSide,
                                                              Units aSignedUnits) {
    if (aSignedUnits == 0 || aSignedUnits == std::numeric_limits<Units>::min())
    {
        return std::nullopt;
    }

    const bool hasExpectedSign = (aTradeSide == TradeSide::Buy && aSignedUnits > 0) ||
                                 (aTradeSide == TradeSide::Sell && aSignedUnits < 0);

    if (!hasExpectedSign)
    {
        return std::nullopt;
    }

    return std::abs(aSignedUnits);
}

std::optional<Money> TradeRepublicParser::parseTaxPaid(std::string_view aValue) {
    if (aValue.empty())
    {
        return Money{0};
    }

    const auto signedTax = parseScaledNumber<Money, MONEY_SCALE>(aValue);

    if (!signedTax || *signedTax > 0 || *signedTax == std::numeric_limits<Money>::min())
    {
        return std::nullopt;
    }

    return std::abs(*signedTax);
}

Currency TradeRepublicParser::parseCurrency(std::string_view aValue) {
    if (aValue == "EUR")
        return Currency::EUR;
    if (aValue == "USD")
        return Currency::USD;
    if (aValue == "GBP")
        return Currency::GBP;
    if (aValue == "CHF")
        return Currency::CHF;
    if (aValue == "JPY")
        return Currency::JPY;
    // add others as needed
    return Currency::Unknown;
}

std::optional<TradeSide> TradeRepublicParser::parseTradeSide(std::string_view aValue) {
    if (aValue == "BUY")
        return TradeSide::Buy;
    if (aValue == "SELL")
        return TradeSide::Sell;
    return std::nullopt;
}

} // namespace taxbroker::tr
