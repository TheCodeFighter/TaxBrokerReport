#include "parsers/traderepublic_parser.hpp"
#include "taxbroker/types.hpp"
#include "utils/logger.hpp"
#include "utils/numeric_util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <csv.hpp>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
struct TransactionTypeMapping {
    std::string_view mType;
    std::string_view mExpectedCategory;
    taxbroker::RowType mRowType;
};

constexpr auto kTransactionTypeMappings = std::array{
    TransactionTypeMapping{"BUY", "TRADING", taxbroker::RowType::Trade},
    TransactionTypeMapping{"SELL", "TRADING", taxbroker::RowType::Trade},
    TransactionTypeMapping{"SAVINGS_PLAN_EXECUTED", "TRADING", taxbroker::RowType::Trade},
    TransactionTypeMapping{"BENEFITS_SPARE_CHANGE_EXECUTION", "TRADING", taxbroker::RowType::Trade},
    TransactionTypeMapping{"BENEFITS_SAVEBACK_EXECUTION", "TRADING", taxbroker::RowType::Trade},
    TransactionTypeMapping{"DIVIDEND", "CASH", taxbroker::RowType::Dividend},
    TransactionTypeMapping{"DISTRIBUTION", "CASH", taxbroker::RowType::Dividend},
    TransactionTypeMapping{"INTEREST_PAYMENT", "CASH", taxbroker::RowType::Interest},
    TransactionTypeMapping{"BOND_INTEREST", "CASH", taxbroker::RowType::Interest},
    TransactionTypeMapping{"FIXED_INCOME", "CASH", taxbroker::RowType::Unsupported},
    TransactionTypeMapping{"SPLIT", "CORPORATE_ACTION", taxbroker::RowType::CorporateAction},

    TransactionTypeMapping{"BENEFITS_SAVEBACK", "CASH", taxbroker::RowType::Benefit},
    TransactionTypeMapping{"STOCKPERK", "CASH", taxbroker::RowType::Benefit},

    TransactionTypeMapping{"BONUS", "CASH", taxbroker::RowType::PrivateMarket},
    TransactionTypeMapping{"PRIVATE_MARKET_BUY", "CASH", taxbroker::RowType::PrivateMarket},
    TransactionTypeMapping{"PRIVATE_MARKET_SELL", "CASH", taxbroker::RowType::PrivateMarket},

    TransactionTypeMapping{"TAX_OPTIMIZATION", "", taxbroker::RowType::Unsupported},
    TransactionTypeMapping{"TAX_REFUND", "", taxbroker::RowType::Unsupported},
    TransactionTypeMapping{"SSP_TAX_CORRECTION_INVOICE", "", taxbroker::RowType::Unsupported},
    TransactionTypeMapping{
        "SSP_CORPORATE_ACTION_INVOICE_CASH", "", taxbroker::RowType::Unsupported},
    TransactionTypeMapping{"WARRANT_EXERCISE", "", taxbroker::RowType::Unsupported},
    TransactionTypeMapping{"TILG", "", taxbroker::RowType::Unsupported},
    TransactionTypeMapping{"CRYPTO_INVOICE", "", taxbroker::RowType::Unsupported},

    TransactionTypeMapping{"CARD_FAILED_TRANSACTION", "CASH", taxbroker::RowType::Ignored},
    TransactionTypeMapping{"CARD_ORDER_BILLED", "CASH", taxbroker::RowType::Ignored},
    TransactionTypeMapping{"CARD_ORDERING_FEE", "CASH", taxbroker::RowType::Ignored},
    TransactionTypeMapping{"CARD_TRANSACTION", "CASH", taxbroker::RowType::Ignored},
    TransactionTypeMapping{"CARD_TRANSACTION_INTERNATIONAL", "CASH", taxbroker::RowType::Ignored},
    TransactionTypeMapping{"CUSTOMER_INBOUND", "CASH", taxbroker::RowType::Ignored},
    TransactionTypeMapping{"CUSTOMER_INPAYMENT", "CASH", taxbroker::RowType::Ignored},
    TransactionTypeMapping{"CUSTOMER_OUTBOUND_REQUEST", "CASH", taxbroker::RowType::Ignored},
    TransactionTypeMapping{"TRANSFER_INSTANT_INBOUND", "CASH", taxbroker::RowType::Ignored},
    TransactionTypeMapping{"TRANSFER_INSTANT_OUTBOUND", "CASH", taxbroker::RowType::Ignored},
    TransactionTypeMapping{"TRANSFER_OUTBOUND", "CASH", taxbroker::RowType::Ignored},
    TransactionTypeMapping{"CREDIT", "CASH", taxbroker::RowType::Ignored},
};

void addWarning(taxbroker::ParseResult& aParseResult,
                const std::filesystem::path& aSourceFile,
                std::size_t aRowIndex,
                taxbroker::WarningCode aWarningCode,
                std::string aMessage) {
    LOG_WARNING("CSV row {}: {}", aRowIndex, aMessage);
    aParseResult.mWarnings.emplace_back(taxbroker::ParseWarning{
        .mCode = aWarningCode,
        .mSourceFile = aSourceFile.string(),
        .mRowIndex = aRowIndex,
        .mMessage = std::move(aMessage),
    });
}

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

std::string getAmountCurrencyValue(const csv::CSVRow& aCsvRow) {
    const auto originalCurrency = aCsvRow["original_currency"].get<std::string>();
    return originalCurrency.empty() ? aCsvRow["currency"].get<std::string>() : originalCurrency;
}
} // namespace

namespace taxbroker::tr {

ParseResult TradeRepublicParser::parse(const std::filesystem::path& aCsvPath) {
    csv::CSVFormat format;
    format.delimiter(delimiter);
    csv::CSVReader reader(aCsvPath.string(), format);

    ParseResult parsedResult;

    std::size_t rowIndex = 1;
    for (csv::CSVRow& row : reader)
    {
        ++rowIndex;
        const RowMeta rowMeta = detectRowType(row);

        switch (rowMeta.mRowType)
        {
        case RowType::Trade: {
            const bool wasParsed = parseTradeRow(row,
                                                 parsedResult.mStatement.mTradeInstruments,
                                                 rowMeta.mParsedValues);
            const auto assetClass = parseAssetClass(rowMeta.mParsedValues.mAssetClass);
            if (wasParsed &&
                (assetClass == AssetClass::PrivateFund || assetClass == AssetClass::Crypto))
            {
                addWarning(parsedResult,
                           aCsvPath,
                           rowIndex,
                           WarningCode::UnsupportedAssetClass,
                           "Trade Republic asset class '" + rowMeta.mParsedValues.mAssetClass +
                               "' is preserved, but its tax treatment is not supported yet "
                               "(transaction ID '" +
                               rowMeta.mParsedValues.mTransactionId + "').");
            }

            break;
        }
        case RowType::Dividend:
            parseDividendRow(row, parsedResult.mStatement.mDividendInstruments);
            break;
        case RowType::Interest: {
            const auto interestType = detectInterestType(rowMeta.mParsedValues.mType);
            parseInterestRow(row, parsedResult.mStatement.mInterestInstruments, interestType);
            break;
        }
        case RowType::CorporateAction:
            parseCorporateActionRow(row, parsedResult.mStatement.mTradeInstruments);
            break;
        case RowType::Benefit:
            parseBenefitRow(row, parsedResult.mStatement.mBenefitEvents, rowMeta.mParsedValues);
            break;
        case RowType::PrivateMarket:
            parsePrivateMarketRow(row,
                                  parsedResult.mStatement.mPrivateMarketEvents,
                                  rowMeta.mParsedValues);
            break;
        case RowType::Ignored:
            break;
        case RowType::Unsupported:
            addWarning(parsedResult,
                       aCsvPath,
                       rowIndex,
                       WarningCode::UnsupportedRowType,
                       "Trade Republic transaction type '" + rowMeta.mParsedValues.mType +
                           "' in category '" + rowMeta.mParsedValues.mCategory +
                           "' is recognized but not supported yet (transaction ID '" +
                           rowMeta.mParsedValues.mTransactionId + "').");
            break;
        case RowType::Unknown:
            addWarning(parsedResult,
                       aCsvPath,
                       rowIndex,
                       WarningCode::UnknownRowType,
                       "Unknown Trade Republic transaction type '" + rowMeta.mParsedValues.mType +
                           "' in category '" + rowMeta.mParsedValues.mCategory +
                           "' (transaction ID '" + rowMeta.mParsedValues.mTransactionId + "').");
            break;
        }
    }

    return parsedResult;
}

RowMeta TradeRepublicParser::detectRowType(const csv::CSVRow& aCsvRow) const {
    RowType rowType = RowType::Unknown;

    auto category = aCsvRow["category"].get<std::string>();
    auto type = aCsvRow["type"].get<std::string>();
    auto assetClass = aCsvRow["asset_class"].get<std::string>();
    auto transactionId = aCsvRow["transaction_id"].get<std::string>();

    const auto mapping = std::find_if(kTransactionTypeMappings.begin(),
                                      kTransactionTypeMappings.end(),
                                      [&](const TransactionTypeMapping& aMapping) {
                                          const bool categoryMatches =
                                              aMapping.mExpectedCategory.empty() ||
                                              aMapping.mExpectedCategory == category;
                                          return aMapping.mType == type && categoryMatches;
                                      });
    if (mapping != kTransactionTypeMappings.end())
    {
        rowType = mapping->mRowType;
    }

    return RowMeta{
        .mRowType = rowType,
        .mParsedValues =
            RowParsedValues{
                .mCategory = std::move(category),
                .mType = std::move(type),
                .mAssetClass = std::move(assetClass),
                .mTransactionId = std::move(transactionId),
            },
    };
}

InterestType TradeRepublicParser::detectInterestType(std::string_view aType) const {
    if (aType == "INTEREST_PAYMENT")
    {
        return InterestType::BrokerInterest;
    }
    // TODO: Check what is actually type for bond interest
    if (aType == "BOND_INTEREST")
    {
        return InterestType::BondInterest;
    }

    return InterestType::UnknownInterest;
}

bool TradeRepublicParser::isInstrumentValid(std::string_view aContext,
                                            const std::string& aIsin,
                                            const std::string& aName) {
    if (aIsin.empty() && aName.empty())
    {
        LOG_WARNING("Missing ISIN and name values for {} row. Skipping row.", aContext);
        return false;
    }

    auto logMainFail = [&](std::string_view aFieldName) {
        LOG_WARNING("Failed to parse {} row: {} is empty. Skipping row.", aContext, aFieldName);
    };

    if (aIsin.empty())
    {
        logMainFail("ISIN");
        return false;
    }

    if (aName.empty())
    {
        logMainFail("name");
        return false;
    }

    return true;
}

GetAmount TradeRepublicParser::getAmountAndCurrency(const csv::CSVRow& aCsvRow) {
    std::optional<Money> grossAmount{};
    std::optional<ExchangeRate> exchangeRate{EXCHANGE_RATE_SCALE};

    const auto originalCurrencyValue = aCsvRow["original_currency"].get<std::string>();
    const bool usesOriginalCurrency = !originalCurrencyValue.empty();
    const auto currency = parseCurrency(
        usesOriginalCurrency ? originalCurrencyValue : aCsvRow["currency"].get<std::string>());

    if (currency == Currency::Unknown)
    {
        exchangeRate.reset();
        return GetAmount{
            .mGrossAmount = grossAmount,
            .mExchangeRate = exchangeRate,
            .mCurrency = std::nullopt,
        };
    }
    if (!usesOriginalCurrency)
    {
        grossAmount = parseMoney(aCsvRow["amount"].get<std::string>());
    }
    else
    {
        grossAmount = parseMoney(aCsvRow["original_amount"].get<std::string>());
        exchangeRate = parseExchangeRate(aCsvRow["fx_rate"].get<std::string>());
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
    if (!aRow["original_currency"].get<std::string>().empty())
    {
        return {"original_amount", originalAmount};
    }

    return {"amount", aRow["amount"].get<std::string>()};
}

bool TradeRepublicParser::parseTradeRow(const csv::CSVRow& aCsvRow,
                                        std::vector<TradeInstrument>& aInstruments,
                                        const RowParsedValues& aParsedValues) {

    const auto isinValue = aCsvRow["symbol"].get<std::string>();
    const auto nameValue = aCsvRow["name"].get<std::string>();
    std::string_view typeValue = aParsedValues.mType;

    if (!isInstrumentValid("trade", isinValue, nameValue))
    {
        return false;
    }

    auto date = parseDate(aCsvRow["date"].get<std::string>());
    auto tradeSide = parseTradeSide(typeValue);
    auto unitPrice = parseMoney(aCsvRow["price"].get<std::string>());
    auto units = parseUnits(aCsvRow["shares"].get<std::string>());
    const auto amountValue = aCsvRow["amount"].get<std::string>();
    auto amount = amountValue.empty() ? std::optional<Money>{} : parseMoney(amountValue);
    auto feePaid = parseFeePaid(aCsvRow["fee"].get<std::string>());
    auto currency = parseCurrency(aCsvRow["currency"].get<std::string>());
    auto assetClass = parseAssetClass(aCsvRow["asset_class"].get<std::string>());

    auto logFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse {} value: {} for row with ISIN {}",
                    aFieldName,
                    aValue,
                    isinValue);
    };

    if (!date)
    {
        logFail("date", aCsvRow["date"].get<std::string>());
        return false;
    }

    if (!tradeSide)
    {
        logFail("trade side", typeValue);
        return false;
    }

    if (!unitPrice || *unitPrice <= 0)
    {
        logFail("unit price", aCsvRow["price"].get<std::string>());
        return false;
    }

    if (!units)
    {
        logFail("units", aCsvRow["shares"].get<std::string>());
        return false;
    }

    const auto normalizedUnits = normalizeTradeUnits(*tradeSide, *units);
    if (!normalizedUnits)
    {
        logFail("units inconsistent with trade side", aCsvRow["shares"].get<std::string>());
        return false;
    }

    if (!amountValue.empty() && !amount)
    {
        logFail("amount", amountValue);
        return false;
    }

    if (amount)
    {
        amount = normalizeTradeAmount(*tradeSide, *amount);
        if (!amount)
        {
            logFail("amount inconsistent with trade side", amountValue);
            return false;
        }
    }

    if (!feePaid)
    {
        logFail("fee", aCsvRow["fee"].get<std::string>());
        return false;
    }

    if (currency == Currency::Unknown)
    {
        logFail("currency", aCsvRow["currency"].get<std::string>());
        return false;
    }

    if (assetClass == AssetClass::Unknown)
    {
        logFail("asset class", aCsvRow["asset_class"].get<std::string>());
        return false;
    }

    auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);
    if (instrument.mAssetClass != AssetClass::Unknown && instrument.mAssetClass != assetClass)
    {
        logFail("asset class inconsistent with existing instrument",
                aCsvRow["asset_class"].get<std::string>());
        return false;
    }
    instrument.mAssetClass = assetClass;

    instrument.mTransactions.emplace_back(TradeTransaction{
        .mDate = *date,
        .mTradeSide = *tradeSide,
        .mUnitPrice = *unitPrice,
        .mUnits = *normalizedUnits,
        .mAmount = amount,
        .mFeePaid = *feePaid,
        .mExchangeRate = EXCHANGE_RATE_SCALE,
        .mCurrency = currency,
        .mTransactionId = aParsedValues.mTransactionId,
    });

    return true;
}

void TradeRepublicParser::parseDividendRow(const csv::CSVRow& aCsvRow,
                                           std::vector<DividendInstrument>& aInstruments) {
    const auto isinValue = aCsvRow["symbol"].get<std::string>();
    const auto nameValue = aCsvRow["name"].get<std::string>();

    if (!isInstrumentValid("dividend", isinValue, nameValue))
    {
        return;
    }

    auto date = parseDate(aCsvRow["date"].get<std::string>());
    auto taxPaid = parseTaxPaid(aCsvRow["tax"].get<std::string>());
    auto taxCurrency = parseCurrency(aCsvRow["currency"].get<std::string>());
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

    if (taxCurrency == Currency::Unknown)
    {
        logFail("tax currency", aCsvRow["currency"].get<std::string>());
        return;
    }

    if (!amountAndCurrency.mCurrency.has_value())
    {
        logFail("currency", getAmountCurrencyValue(aCsvRow));
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

    instrument.mTransactions.emplace_back(DividendTransaction{
        .mDate = *date,
        .mGrossAmount = *amountAndCurrency.mGrossAmount,
        .mTaxPaid = *taxPaid,
        .mExchangeRate = *amountAndCurrency.mExchangeRate,
        .mCurrency = *amountAndCurrency.mCurrency,
        .mTaxCurrency = taxCurrency,
        .mTransactionId = aCsvRow["transaction_id"].get<std::string>(),
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
        auto taxCurrency = parseCurrency(aCsvRow["currency"].get<std::string>());

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

        if (taxCurrency == Currency::Unknown)
        {
            logFail("tax currency", aCsvRow["currency"].get<std::string>());
            return;
        }

        if (!amountAndCurrency.mCurrency.has_value())
        {
            logFail("currency", getAmountCurrencyValue(aCsvRow));
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
            .mTaxCurrency = taxCurrency,
            .mTransactionId = aCsvRow["transaction_id"].get<std::string>(),
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
            auto date = parseDate(aCsvRow["date"].get<std::string>());
            auto taxPaid = parseTaxPaid(aCsvRow["tax"].get<std::string>());
            auto taxCurrency = parseCurrency(aCsvRow["currency"].get<std::string>());

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

            if (taxCurrency == Currency::Unknown)
            {
                logFail("tax currency", aCsvRow["currency"].get<std::string>());
                return;
            }

            if (!amountAndCurrency.mCurrency.has_value())
            {
                logFail("currency", getAmountCurrencyValue(aCsvRow));
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

            instrumentIt->mTransactions.emplace_back(InterestTransaction{
                .mDate = *date,
                .mGrossAmount = *amountAndCurrency.mGrossAmount,
                .mTaxPaid = *taxPaid,
                .mExchangeRate = *amountAndCurrency.mExchangeRate,
                .mCurrency = *amountAndCurrency.mCurrency,
                .mTaxCurrency = taxCurrency,
                .mTransactionId = aCsvRow["transaction_id"].get<std::string>()});
        }

        // Broker interest
    }
}

void TradeRepublicParser::parseCorporateActionRow(const csv::CSVRow& aCsvRow,
                                                  std::vector<TradeInstrument>& aInstruments) {
    const auto isinValue = aCsvRow["symbol"].get<std::string>();
    const auto nameValue = aCsvRow["name"].get<std::string>();

    if (!isInstrumentValid("corporate action", isinValue, nameValue))
    {
        return;
    }

    const auto date = parseDate(aCsvRow["date"].get<std::string>());
    const auto unitsDelta = parseUnits(aCsvRow["shares"].get<std::string>());
    const auto assetClass = parseAssetClass(aCsvRow["asset_class"].get<std::string>());

    auto logFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse {} value: {} for corporate action row with ISIN {}",
                    aFieldName,
                    aValue,
                    isinValue);
    };

    if (!date)
    {
        logFail("date", aCsvRow["date"].get<std::string>());
        return;
    }

    if (!unitsDelta || *unitsDelta == 0)
    {
        logFail("units delta", aCsvRow["shares"].get<std::string>());
        return;
    }

    if (assetClass == AssetClass::Unknown)
    {
        logFail("asset class", aCsvRow["asset_class"].get<std::string>());
        return;
    }

    auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);
    if (instrument.mAssetClass != AssetClass::Unknown && instrument.mAssetClass != assetClass)
    {
        logFail("asset class inconsistent with existing instrument",
                aCsvRow["asset_class"].get<std::string>());
        return;
    }
    instrument.mAssetClass = assetClass;

    // Trade Republic exports the signed change in units, not the split ratio. The ratio can only
    // be derived after statements are merged and the open position immediately before this event
    // is known.
    instrument.mCorporateActions.emplace_back(CorporateAction{
        .mDate = *date,
        .mType = *unitsDelta > 0 ? CorporateActionType::Split : CorporateActionType::ReverseSplit,
        .mUnitsDelta = *unitsDelta,
        .mRatio = std::nullopt,
        .mTransactionId = aCsvRow["transaction_id"].get<std::string>(),
    });
}

void TradeRepublicParser::parseBenefitRow(const csv::CSVRow& aCsvRow,
                                          std::vector<BenefitEvent>& aBenefitEvents,
                                          const RowParsedValues& aParsedValues) {
    const auto date = parseDate(aCsvRow["date"].get<std::string>());
    const auto benefitType = parseBenefitType(aParsedValues.mType);
    const auto amount = parseMoney(aCsvRow["amount"].get<std::string>());
    const auto currency = parseCurrency(aCsvRow["currency"].get<std::string>());

    auto logFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse {} value: {} for Trade Republic benefit row",
                    aFieldName,
                    aValue);
    };

    if (!date)
    {
        logFail("date", aCsvRow["date"].get<std::string>());
        return;
    }

    if (!benefitType)
    {
        logFail("benefit type", aParsedValues.mType);
        return;
    }

    if (!amount)
    {
        logFail("amount", aCsvRow["amount"].get<std::string>());
        return;
    }

    if (currency == Currency::Unknown)
    {
        logFail("currency", aCsvRow["currency"].get<std::string>());
        return;
    }

    const auto isin = aCsvRow["symbol"].get<std::string>();
    aBenefitEvents.emplace_back(BenefitEvent{
        .mDate = *date,
        .mType = *benefitType,
        .mName = aCsvRow["name"].get<std::string>(),
        .mIsin = isin.empty() ? std::nullopt : std::optional<Isin>{isin},
        .mAssetClass = parseAssetClass(aParsedValues.mAssetClass),
        .mAmount = *amount,
        .mCurrency = currency,
        .mTransactionId = aParsedValues.mTransactionId,
    });
}

void TradeRepublicParser::parsePrivateMarketRow(
    const csv::CSVRow& aCsvRow,
    std::vector<PrivateMarketEvent>& aPrivateMarketEvents,
    const RowParsedValues& aParsedValues) {
    const auto date = parseDate(aCsvRow["date"].get<std::string>());
    const auto eventType = parsePrivateMarketEventType(aParsedValues.mType);
    const auto amount = parseMoney(aCsvRow["amount"].get<std::string>());
    const auto feePaid = parseFeePaid(aCsvRow["fee"].get<std::string>());
    const auto currency = parseCurrency(aCsvRow["currency"].get<std::string>());

    auto logFail = [&](std::string_view aFieldName, std::string_view aValue) {
        LOG_WARNING("Failed to parse {} value: {} for Trade Republic private-market row",
                    aFieldName,
                    aValue);
    };

    if (!date)
    {
        logFail("date", aCsvRow["date"].get<std::string>());
        return;
    }

    if (!eventType)
    {
        logFail("private-market event type", aParsedValues.mType);
        return;
    }

    if (!amount)
    {
        logFail("amount", aCsvRow["amount"].get<std::string>());
        return;
    }

    if (!feePaid)
    {
        logFail("fee", aCsvRow["fee"].get<std::string>());
        return;
    }

    if (currency == Currency::Unknown)
    {
        logFail("currency", aCsvRow["currency"].get<std::string>());
        return;
    }

    const auto isin = aCsvRow["symbol"].get<std::string>();
    aPrivateMarketEvents.emplace_back(PrivateMarketEvent{
        .mDate = *date,
        .mType = *eventType,
        .mName = aCsvRow["name"].get<std::string>(),
        .mIsin = isin.empty() ? std::nullopt : std::optional<Isin>{isin},
        .mAssetClass = parseAssetClass(aParsedValues.mAssetClass),
        .mAmount = *amount,
        .mFeePaid = *feePaid,
        .mCurrency = currency,
        .mDescription = aCsvRow["description"].get<std::string>(),
        .mTransactionId = aParsedValues.mTransactionId,
    });
}

std::optional<Date> TradeRepublicParser::parseDate(std::string_view aValue) {
    if (aValue.size() != 10 || aValue[4] != '-' || aValue[7] != '-')
        return std::nullopt;

    int year{}, month{}, day{};
    auto view_year = aValue.substr(0, 4);
    auto view_month = aValue.substr(5, 2);
    auto view_day = aValue.substr(8, 2);

    const auto parsePart = [](std::string_view aPart, int& aResult) {
        const auto end = aPart.data() + aPart.size();
        const auto [parsedEnd, error] = std::from_chars(aPart.data(), end, aResult);
        return error == std::errc{} && parsedEnd == end;
    };

    if (!parsePart(view_year, year) || !parsePart(view_month, month) || !parsePart(view_day, day))
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

std::optional<ExchangeRate> TradeRepublicParser::parseExchangeRate(std::string_view aValue) {
    const auto exchangeRate = parseScaledNumber<ExchangeRate, EXCHANGE_RATE_SCALE>(aValue);
    if (!exchangeRate || *exchangeRate <= 0)
    {
        return std::nullopt;
    }

    return exchangeRate;
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

std::optional<Money> TradeRepublicParser::normalizeTradeAmount(TradeSide aTradeSide,
                                                               Money aSignedAmount) {
    if (aSignedAmount == 0 || aSignedAmount == std::numeric_limits<Money>::min())
    {
        return std::nullopt;
    }

    const bool hasExpectedSign = (aTradeSide == TradeSide::Buy && aSignedAmount < 0) ||
                                 (aTradeSide == TradeSide::Sell && aSignedAmount > 0);
    if (!hasExpectedSign)
    {
        return std::nullopt;
    }

    return std::abs(aSignedAmount);
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

std::optional<Money> TradeRepublicParser::parseFeePaid(std::string_view aValue) {
    if (aValue.empty())
    {
        return Money{0};
    }

    const auto signedFee = parseScaledNumber<Money, MONEY_SCALE>(aValue);
    if (!signedFee || *signedFee > 0 || *signedFee == std::numeric_limits<Money>::min())
    {
        return std::nullopt;
    }

    return std::abs(*signedFee);
}

// TODO: Extend when more currencies are known
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

AssetClass TradeRepublicParser::parseAssetClass(std::string_view aValue) {
    if (aValue == "STOCK")
        return AssetClass::Stock;
    if (aValue == "FUND")
        return AssetClass::Fund;
    if (aValue == "BOND")
        return AssetClass::Bond;
    if (aValue == "DERIVATIVE")
        return AssetClass::Derivative;
    if (aValue == "CRYPTO")
        return AssetClass::Crypto;
    if (aValue == "PRIVATE_FUND")
        return AssetClass::PrivateFund;
    return AssetClass::Unknown;
}

std::optional<BenefitType> TradeRepublicParser::parseBenefitType(std::string_view aValue) {
    if (aValue == "BENEFITS_SAVEBACK")
        return BenefitType::Saveback;
    if (aValue == "STOCKPERK")
        return BenefitType::Stockperk;
    return std::nullopt;
}

std::optional<PrivateMarketEventType>
TradeRepublicParser::parsePrivateMarketEventType(std::string_view aValue) {
    if (aValue == "PRIVATE_MARKET_BUY")
        return PrivateMarketEventType::Buy;
    if (aValue == "PRIVATE_MARKET_SELL")
        return PrivateMarketEventType::Sell;
    if (aValue == "BONUS")
        return PrivateMarketEventType::Bonus;
    return std::nullopt;
}

std::optional<TradeSide> TradeRepublicParser::parseTradeSide(std::string_view aValue) {
    if (aValue == "BUY" || aValue == "SAVINGS_PLAN_EXECUTED" ||
        aValue == "BENEFITS_SPARE_CHANGE_EXECUTION" || aValue == "BENEFITS_SAVEBACK_EXECUTION")
        return TradeSide::Buy;
    if (aValue == "SELL")
        return TradeSide::Sell;
    return std::nullopt;
}

} // namespace taxbroker::tr
