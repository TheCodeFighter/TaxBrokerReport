#include "parsers/traderepublic_parser.hpp"
#include "taxbroker/types.hpp"
#include "utils/date_utils.hpp"
#include "utils/logger.hpp"
#include "utils/numeric_util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csv.hpp>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
static_assert(TR_DATE_MISMATCH_DEFAULT_HOUR >= 0 && TR_DATE_MISMATCH_DEFAULT_HOUR < 24);

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
};

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

struct TradeRepublicParser::RowContext {
    ParseResult& mParseResult;
    const SourceFilename& mSourceFile;
    std::size_t mRowIndex;
    StableInputSequence mInputSequence;
    std::string_view mTransactionId;
    std::optional<SourceTimestamp> mSourceTimestamp;
    std::optional<Date> mSourceCalendarDate;
    bool mSourceTimestampInvalid{};

    [[nodiscard]] EventMetadata metadata(Date aTaxDate) const;

    void addDiagnostic(DiagnosticSeverity aSeverity,
                       DiagnosticCode aCode,
                       std::string aMessage,
                       std::optional<std::string> aField = std::nullopt) const;

    void addInvalidFieldDiagnostic(std::string_view aField,
                                   std::string_view aValue,
                                   std::string_view aRowKind) const;
};

struct TradeRepublicParser::ParsedIncome {
    Date mDate;
    Money mGrossAmount;
    std::optional<Money> mTaxPaid;
    std::optional<ExchangeRate> mExchangeRate;
    Currency mCurrency;
    Currency mTaxCurrency;
};

void TradeRepublicParser::RowContext::addDiagnostic(DiagnosticSeverity aSeverity,
                                                    DiagnosticCode aCode,
                                                    std::string aMessage,
                                                    std::optional<std::string> aField) const {
    if (aSeverity == DiagnosticSeverity::Warning)
    {
        LOG_WARNING("Trade Republic CSV source {} row {}: {}",
                    mInputSequence.mSourceIndex,
                    mRowIndex,
                    aMessage);
    }
    else
    {
        LOG_ERROR("Trade Republic CSV source {} row {}: {}",
                  mInputSequence.mSourceIndex,
                  mRowIndex,
                  aMessage);
    }

    std::optional<std::string> storedTransactionId;
    if (!mTransactionId.empty())
    {
        storedTransactionId.emplace(mTransactionId);
    }

    mParseResult.mDiagnostics.emplace_back(ParseDiagnostic{
        .mSeverity = aSeverity,
        .mCode = aCode,
        .mSourceFile = mSourceFile.value(),
        .mRowIndex = mRowIndex,
        .mTransactionId = std::move(storedTransactionId),
        .mField = std::move(aField),
        .mMessage = std::move(aMessage),
    });
}

void TradeRepublicParser::RowContext::addInvalidFieldDiagnostic(std::string_view aField,
                                                                std::string_view aValue,
                                                                std::string_view aRowKind) const {
    const bool isMissing = aValue.empty();
    addDiagnostic(DiagnosticSeverity::Error,
                  isMissing ? DiagnosticCode::MissingField : DiagnosticCode::InvalidValue,
                  isMissing ? "Required field '" + std::string{aField} + "' is missing in " +
                                  std::string{aRowKind} + "; the row was skipped."
                            : "Field '" + std::string{aField} + "' has an invalid value in " +
                                  std::string{aRowKind} + "; the row was skipped.",
                  std::string{aField});
}

EventMetadata TradeRepublicParser::RowContext::metadata(Date aTaxDate) const {
    std::optional<SourceTimestamp> orderingTimestamp;

    if (mSourceTimestampInvalid)
    {
        addDiagnostic(DiagnosticSeverity::Warning,
                      DiagnosticCode::InvalidValue,
                      "Field 'datetime' is invalid; exact transaction order is unknown. "
                      "The event retains its tax date and uses deterministic source ordering.",
                      "datetime");
    }

    if (mSourceCalendarDate && *mSourceCalendarDate != aTaxDate)
    {
        orderingTimestamp = SourceTimestamp{aTaxDate.time_since_epoch() +
                                            std::chrono::hours{TR_DATE_MISMATCH_DEFAULT_HOUR}};

        addDiagnostic(
            DiagnosticSeverity::Warning,
            DiagnosticCode::InconsistentValue,
            "The calendar date in 'datetime' differs from 'date'; reporting uses 'date' "
            "and ordering uses that date at the default hour " +
                std::to_string(TR_DATE_MISMATCH_DEFAULT_HOUR) +
                ":00:00 UTC. The original timestamp is retained. FIFO or corporate-action "
                "order may be incorrect; review the transaction order.",
            "datetime");
    }

    return EventMetadata{
        .mTaxDate = aTaxDate,
        .mSourceTimestamp = mSourceTimestamp,
        .mOrderingTimestamp = orderingTimestamp,
        .mSources = {SourceReference{
            .mBroker = Broker::TradeRepublic,
            .mFilename = mSourceFile,
            .mSourceRow = mRowIndex,
            .mTransactionId =
                mTransactionId.empty() ? std::nullopt : std::optional<std::string>{mTransactionId},
            .mInputSequence = mInputSequence,
        }},
    };
}

ParseResult TradeRepublicParser::parse(const std::filesystem::path& aCsvPath,
                                       std::size_t aSourceIndex) {
    ParseResult parsedResult{
        .mBroker = Broker::TradeRepublic,
    };
    const auto sourceFile = SourceFilename::fromPath(aCsvPath.string());

    try
    {
        csv::CSVFormat format;
        format.delimiter(delimiter);
        csv::CSVReader reader(aCsvPath.string(), format);

        std::size_t rowIndex = 1;
        for (const csv::CSVRow& row : reader)
        {
            ++rowIndex;
            const RowMeta rowMeta = detectRowType(row);
            const auto datetimeValue = row["datetime"].get<std::string>();
            const auto sourceTimestamp = parseSourceTimestamp(datetimeValue);
            const RowContext context{
                .mParseResult = parsedResult,
                .mSourceFile = sourceFile,
                .mRowIndex = rowIndex,
                .mInputSequence =
                    StableInputSequence{
                        .mSourceIndex = aSourceIndex,
                        .mEventIndex = rowIndex - 2,
                    },
                .mTransactionId = rowMeta.mParsedValues.mTransactionId,
                .mSourceTimestamp = sourceTimestamp,
                .mSourceCalendarDate =
                    sourceTimestamp
                        ? parseCalendarDate(std::string_view{datetimeValue}.substr(0, 10))
                        : std::nullopt,
                .mSourceTimestampInvalid = !datetimeValue.empty() && !sourceTimestamp,
            };

            const bool isImportedEvent =
                rowMeta.mRowType == RowType::Trade || rowMeta.mRowType == RowType::Dividend ||
                rowMeta.mRowType == RowType::Interest ||
                rowMeta.mRowType == RowType::CorporateAction ||
                rowMeta.mRowType == RowType::Benefit || rowMeta.mRowType == RowType::PrivateMarket;

            if (isImportedEvent && datetimeValue.empty())
            {
                context.addDiagnostic(DiagnosticSeverity::Error,
                                      DiagnosticCode::MissingField,
                                      "Required field 'datetime' is missing; transaction order "
                                      "cannot be established. "
                                      "The row was skipped; review the source transaction.",
                                      "datetime");
                continue;
            }

            switch (rowMeta.mRowType)
            {
            case RowType::Trade: {
                const bool wasParsed = parseTradeRow(row,
                                                     parsedResult.mStatement.mTradeInstruments,
                                                     rowMeta.mParsedValues,
                                                     context);
                const auto assetClass = parseAssetClass(rowMeta.mParsedValues.mAssetClass);
                if (wasParsed &&
                    (assetClass == AssetClass::PrivateFund || assetClass == AssetClass::Crypto))
                {
                    context.addDiagnostic(
                        DiagnosticSeverity::Warning,
                        DiagnosticCode::UnsupportedAssetClass,
                        "Asset class '" + rowMeta.mParsedValues.mAssetClass +
                            "' was preserved, but its tax treatment is not supported yet.",
                        "asset_class");
                }

                break;
            }
            case RowType::Dividend:
                parseDividendRow(row, parsedResult.mStatement.mDividendInstruments, context);
                break;
            case RowType::Interest: {
                const auto interestType = detectInterestType(row);
                parseInterestRow(row,
                                 parsedResult.mStatement.mInterestInstruments,
                                 interestType,
                                 context);
                break;
            }
            case RowType::CorporateAction:
                parseCorporateActionRow(row, parsedResult.mStatement.mTradeInstruments, context);
                break;
            case RowType::Benefit:
                parseBenefitRow(row,
                                parsedResult.mStatement.mBenefitEvents,
                                rowMeta.mParsedValues,
                                context);
                break;
            case RowType::PrivateMarket:
                parsePrivateMarketRow(row,
                                      parsedResult.mStatement.mPrivateMarketEvents,
                                      rowMeta.mParsedValues,
                                      context);
                break;
            case RowType::Ignored:
                break;
            case RowType::Unsupported:
                context.addDiagnostic(DiagnosticSeverity::Error,
                                      DiagnosticCode::UnsupportedRowType,
                                      "Transaction type '" + rowMeta.mParsedValues.mType +
                                          "' in category '" + rowMeta.mParsedValues.mCategory +
                                          "' is recognized but not supported; the row was skipped.",
                                      "type");
                break;
            case RowType::Unknown:
                context.addDiagnostic(DiagnosticSeverity::Error,
                                      DiagnosticCode::UnknownRowType,
                                      "Transaction type '" + rowMeta.mParsedValues.mType +
                                          "' in category '" + rowMeta.mParsedValues.mCategory +
                                          "' is unknown; the row was skipped.",
                                      "type");
                break;
            }
        }
    } catch (const std::runtime_error&)
    {
        LOG_ERROR("Trade Republic CSV source {} could not be parsed", aSourceIndex);
        // Discard partial results after a file-level failure.
        parsedResult.mStatement = {};
        parsedResult.mDiagnostics.emplace_back(ParseDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = DiagnosticCode::ParseError,
            .mSourceFile = sourceFile.value(),
            .mMessage = "The CSV file could not be opened or did not match the expected Trade "
                        "Republic format.",
        });
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

InterestType TradeRepublicParser::detectInterestType(const csv::CSVRow& aCsvRow) const {
    const auto assetClass = aCsvRow["asset_class"].get<std::string>();

    if (assetClass == "BOND")
    {
        return InterestType::BondInterest;
    }
    if (assetClass.empty() && aCsvRow["symbol"].get<std::string>().empty() &&
        aCsvRow["name"].get<std::string>().empty())
    {
        return InterestType::BrokerInterest;
    }

    return InterestType::UnknownInterest;
}

bool TradeRepublicParser::isInstrumentValid(std::string_view aContext,
                                            const std::string& aIsin,
                                            const std::string& aName,
                                            const RowContext& aRowContext) {
    if (aIsin.empty() && aName.empty())
    {
        aRowContext.addDiagnostic(DiagnosticSeverity::Error,
                                  DiagnosticCode::MissingField,
                                  "Required fields 'symbol' and 'name' are missing in " +
                                      std::string{aContext} + "; the row was skipped.");
        return false;
    }

    if (aIsin.empty())
    {
        aRowContext.addInvalidFieldDiagnostic("symbol", aIsin, aContext);
        return false;
    }

    if (aName.empty())
    {
        aRowContext.addInvalidFieldDiagnostic("name", aName, aContext);
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

    if (grossAmount && *grossAmount < 0)
    {
        grossAmount.reset();
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
                                        const RowParsedValues& aParsedValues,
                                        const RowContext& aContext) {

    const auto isinValue = aCsvRow["symbol"].get<std::string>();
    const auto nameValue = aCsvRow["name"].get<std::string>();
    std::string_view typeValue = aParsedValues.mType;

    if (!isInstrumentValid("trade row", isinValue, nameValue, aContext))
    {
        return false;
    }

    auto date = parseCalendarDate(aCsvRow["date"].get<std::string>());
    auto tradeSide = parseTradeSide(typeValue);
    auto unitPrice = parseMoney(aCsvRow["price"].get<std::string>());
    const auto sharesText = aCsvRow["shares"].get<std::string>();
    const auto importedUnits = importFixedPoint(sharesText, UNITS_SCALE);
    const auto* units = std::get_if<ParsedFixedPoint>(&importedUnits);
    const auto amountValue = aCsvRow["amount"].get<std::string>();
    auto amount = amountValue.empty() ? std::optional<Money>{} : parseMoney(amountValue);
    auto feePaid = parseFeePaid(aCsvRow["fee"].get<std::string>());
    auto currency = parseCurrency(aCsvRow["currency"].get<std::string>());
    auto assetClass = parseAssetClass(aCsvRow["asset_class"].get<std::string>());

    if (!date)
    {
        aContext.addInvalidFieldDiagnostic("date", aCsvRow["date"].get<std::string>(), "trade row");
        return false;
    }

    if (!tradeSide)
    {
        aContext.addInvalidFieldDiagnostic("type", typeValue, "trade row");
        return false;
    }

    if (!unitPrice || *unitPrice <= 0)
    {
        aContext.addInvalidFieldDiagnostic("price",
                                           aCsvRow["price"].get<std::string>(),
                                           "trade row");
        return false;
    }

    if (!units)
    {
        aContext.addInvalidFieldDiagnostic("shares",
                                           aCsvRow["shares"].get<std::string>(),
                                           "trade row");
        return false;
    }

    // Normalize stored trade units; evidence retains the original source sign.
    const auto normalizedUnits = normalizeTradeUnits(*tradeSide, units->mValue);
    if (!normalizedUnits)
    {
        aContext.addDiagnostic(
            DiagnosticSeverity::Error,
            DiagnosticCode::InconsistentValue,
            "Field 'shares' is inconsistent with the trade side; the row was skipped.",
            "shares");
        return false;
    }

    if (!amountValue.empty() && !amount)
    {
        aContext.addInvalidFieldDiagnostic("amount", amountValue, "trade row");
        return false;
    }

    if (amount)
    {
        amount = normalizeTradeAmount(*tradeSide, *amount);
        if (!amount)
        {
            aContext.addDiagnostic(
                DiagnosticSeverity::Error,
                DiagnosticCode::InconsistentValue,
                "Field 'amount' is inconsistent with the trade side; the row was skipped.",
                "amount");
            return false;
        }
    }

    if (!feePaid)
    {
        aContext.addInvalidFieldDiagnostic("fee", aCsvRow["fee"].get<std::string>(), "trade row");
        return false;
    }

    if (currency == Currency::Unknown)
    {
        aContext.addInvalidFieldDiagnostic("currency",
                                           aCsvRow["currency"].get<std::string>(),
                                           "trade row");
        return false;
    }

    if (assetClass == AssetClass::Unknown)
    {
        aContext.addInvalidFieldDiagnostic("asset_class",
                                           aCsvRow["asset_class"].get<std::string>(),
                                           "trade row");
        return false;
    }

    auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);
    if (instrument.mAssetClass != AssetClass::Unknown && instrument.mAssetClass != assetClass)
    {
        aContext.addDiagnostic(
            DiagnosticSeverity::Error,
            DiagnosticCode::InconsistentValue,
            "Field 'asset_class' conflicts with an earlier row for the same symbol; the "
            "row was skipped.",
            "asset_class");
        return false;
    }
    instrument.mAssetClass = assetClass;

    auto metadata = aContext.metadata(*date);
    auto evidence = unitSourceEvidence(*units, sharesText, primarySource(metadata));

    instrument.mTransactions.emplace_back(TradeTransaction{
        .mMetadata = std::move(metadata),
        .mTradeSide = *tradeSide,
        .mUnitPrice = *unitPrice,
        .mUnits = *normalizedUnits,
        .mAmount = amount,
        .mFeePaid = *feePaid,
        .mExchangeRate = EXCHANGE_RATE_SCALE,
        .mCurrency = currency,
        .mUnitEvidence = {std::move(evidence)},
    });

    return true;
}

std::optional<TradeRepublicParser::ParsedIncome> TradeRepublicParser::parseIncome(
    const csv::CSVRow& aCsvRow, std::string_view aRowKind, const RowContext& aContext) {
    const auto dateText = aCsvRow["date"].get<std::string>();
    const auto taxText = aCsvRow["tax"].get<std::string>();
    const auto currencyText = aCsvRow["currency"].get<std::string>();
    const auto rateText = aCsvRow["fx_rate"].get<std::string>();
    const auto date = parseCalendarDate(dateText);
    const auto taxPaid = parseTaxPaid(taxText);
    const auto taxCurrency = parseCurrency(currencyText);
    const auto amountAndCurrency = getAmountAndCurrency(aCsvRow);

    if (!date)
    {
        aContext.addInvalidFieldDiagnostic("date", dateText, aRowKind);
        return std::nullopt;
    }
    if (!taxPaid && !taxText.empty())
    {
        aContext.addInvalidFieldDiagnostic("tax", taxText, aRowKind);
        return std::nullopt;
    }
    if (taxCurrency == Currency::Unknown)
    {
        aContext.addInvalidFieldDiagnostic("currency", currencyText, aRowKind);
        return std::nullopt;
    }
    if (!amountAndCurrency.mCurrency)
    {
        const auto field = aCsvRow["original_currency"].get<std::string>().empty()
                               ? "currency"
                               : "original_currency";

        aContext.addInvalidFieldDiagnostic(field, getAmountCurrencyValue(aCsvRow), aRowKind);
        return std::nullopt;
    }
    if (!amountAndCurrency.mExchangeRate && !rateText.empty())
    {
        aContext.addInvalidFieldDiagnostic("fx_rate", rateText, aRowKind);
        return std::nullopt;
    }
    if (!amountAndCurrency.mGrossAmount)
    {
        const auto [fieldName, fieldValue] = pickAmountField(aCsvRow);

        aContext.addInvalidFieldDiagnostic(fieldName, fieldValue, aRowKind);
        return std::nullopt;
    }

    return ParsedIncome{
        .mDate = *date,
        .mGrossAmount = *amountAndCurrency.mGrossAmount,
        .mTaxPaid = taxPaid,
        .mExchangeRate = amountAndCurrency.mExchangeRate,
        .mCurrency = *amountAndCurrency.mCurrency,
        .mTaxCurrency = taxCurrency,
    };
}

void TradeRepublicParser::parseDividendRow(const csv::CSVRow& aCsvRow,
                                           std::vector<DividendInstrument>& aInstruments,
                                           const RowContext& aContext) {
    const auto isinValue = aCsvRow["symbol"].get<std::string>();
    const auto nameValue = aCsvRow["name"].get<std::string>();

    if (!isInstrumentValid("dividend row", isinValue, nameValue, aContext))
    {
        return;
    }

    const auto income = parseIncome(aCsvRow, "dividend row", aContext);

    if (!income)
    {
        return;
    }

    auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);

    instrument.mTransactions.emplace_back(DividendTransaction{
        .mMetadata = aContext.metadata(income->mDate),
        .mGrossAmount = income->mGrossAmount,
        .mTaxPaid = income->mTaxPaid,
        .mExchangeRate = income->mExchangeRate,
        .mCurrency = income->mCurrency,
        .mTaxCurrency = income->mTaxCurrency,
    });
}

void TradeRepublicParser::parseInterestRow(const csv::CSVRow& aCsvRow,
                                           std::vector<InterestInstrument>& aInstruments,
                                           const InterestType aInterestType,
                                           const RowContext& aContext) {
    if (aInterestType == InterestType::UnknownInterest)
    {
        aContext.addDiagnostic(DiagnosticSeverity::Error,
                               DiagnosticCode::UnsupportedAssetClass,
                               "The interest row has no verified cash-interest or bond-coupon "
                               "mapping; the row was skipped.",
                               "asset_class");
        return;
    }
    if (aInterestType == InterestType::OtherInterest)
    {
        aContext.addDiagnostic(
            DiagnosticSeverity::Error,
            DiagnosticCode::UnsupportedRowType,
            "This interest transaction type is not supported; the row was skipped.",
            "type");
        return;
    }

    const bool isBond = aInterestType == InterestType::BondInterest;
    const auto name = isBond ? aCsvRow["name"].get<std::string>() : "Trade Republic";
    const auto isin = isBond ? aCsvRow["symbol"].get<std::string>() : std::string{};
    const auto rowKind = isBond ? "bond-interest row" : "broker-interest row";

    if (isBond && !isInstrumentValid(rowKind, isin, name, aContext))
    {
        return;
    }

    const auto income = parseIncome(aCsvRow, rowKind, aContext);

    if (!income)
    {
        return;
    }

    auto instrument =
        std::find_if(aInstruments.begin(),
                     aInstruments.end(),
                     [&](const InterestInstrument& aInstrument) {
                         return aInstrument.mInterestType == aInterestType &&
                                (isBond ? aInstrument.mIsin == isin : aInstrument.mName == name);
                     });

    if (instrument == aInstruments.end())
    {
        aInstruments.emplace_back(
            InterestInstrument{.mName = name,
                               .mIsin = isBond ? std::optional<Isin>{isin} : std::nullopt,
                               .mInterestType = aInterestType});
        instrument = std::prev(aInstruments.end());
    }

    instrument->mTransactions.emplace_back(InterestTransaction{
        .mMetadata = aContext.metadata(income->mDate),
        .mGrossAmount = income->mGrossAmount,
        .mTaxPaid = income->mTaxPaid,
        .mExchangeRate = income->mExchangeRate,
        .mCurrency = income->mCurrency,
        .mTaxCurrency = income->mTaxCurrency,
    });
}

void TradeRepublicParser::parseCorporateActionRow(const csv::CSVRow& aCsvRow,
                                                  std::vector<TradeInstrument>& aInstruments,
                                                  const RowContext& aContext) {
    const auto isinValue = aCsvRow["symbol"].get<std::string>();
    const auto nameValue = aCsvRow["name"].get<std::string>();

    if (!isInstrumentValid("corporate-action row", isinValue, nameValue, aContext))
    {
        return;
    }

    const auto date = parseCalendarDate(aCsvRow["date"].get<std::string>());
    const auto sharesText = aCsvRow["shares"].get<std::string>();
    const auto importedUnits = importFixedPoint(sharesText, UNITS_SCALE);
    const auto* unitsDelta = std::get_if<ParsedFixedPoint>(&importedUnits);
    const auto assetClass = parseAssetClass(aCsvRow["asset_class"].get<std::string>());

    if (!date)
    {
        aContext.addInvalidFieldDiagnostic("date",
                                           aCsvRow["date"].get<std::string>(),
                                           "corporate-action row");
        return;
    }

    if (!unitsDelta || unitsDelta->mValue == 0)
    {
        aContext.addInvalidFieldDiagnostic("shares",
                                           aCsvRow["shares"].get<std::string>(),
                                           "corporate-action row");
        return;
    }

    if (assetClass == AssetClass::Unknown)
    {
        aContext.addInvalidFieldDiagnostic("asset_class",
                                           aCsvRow["asset_class"].get<std::string>(),
                                           "corporate-action row");
        return;
    }

    auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);
    if (instrument.mAssetClass != AssetClass::Unknown && instrument.mAssetClass != assetClass)
    {
        aContext.addDiagnostic(
            DiagnosticSeverity::Error,
            DiagnosticCode::InconsistentValue,
            "Field 'asset_class' conflicts with an earlier row for the same symbol; the "
            "row was skipped.",
            "asset_class");
        return;
    }
    instrument.mAssetClass = assetClass;

    // TR shares has no verified economic meaning; only a confirmed ratio establishes direction.
    auto metadata = aContext.metadata(*date);
    auto evidence = unitSourceEvidence(*unitsDelta, sharesText, primarySource(metadata));

    instrument.mCorporateActions.emplace_back(CorporateAction{
        .mMetadata = std::move(metadata),
        .mType = CorporateActionType::UnresolvedSplit,
        .mUnitsDelta = unitsDelta->mValue,
        .mRatio = std::nullopt,
        .mUnitEvidence = {std::move(evidence)},
    });
}

void TradeRepublicParser::parseBenefitRow(const csv::CSVRow& aCsvRow,
                                          std::vector<BenefitEvent>& aBenefitEvents,
                                          const RowParsedValues& aParsedValues,
                                          const RowContext& aContext) {
    const auto date = parseCalendarDate(aCsvRow["date"].get<std::string>());
    const auto benefitType = parseBenefitType(aParsedValues.mType);
    const auto amount = parseMoney(aCsvRow["amount"].get<std::string>());
    const auto currency = parseCurrency(aCsvRow["currency"].get<std::string>());

    if (!date)
    {
        aContext.addInvalidFieldDiagnostic("date",
                                           aCsvRow["date"].get<std::string>(),
                                           "benefit row");
        return;
    }

    if (!benefitType)
    {
        aContext.addInvalidFieldDiagnostic("type", aParsedValues.mType, "benefit row");
        return;
    }

    if (!amount)
    {
        aContext.addInvalidFieldDiagnostic("amount",
                                           aCsvRow["amount"].get<std::string>(),
                                           "benefit row");
        return;
    }

    if (currency == Currency::Unknown)
    {
        aContext.addInvalidFieldDiagnostic("currency",
                                           aCsvRow["currency"].get<std::string>(),
                                           "benefit row");
        return;
    }

    const auto isin = aCsvRow["symbol"].get<std::string>();
    aBenefitEvents.emplace_back(BenefitEvent{
        .mMetadata = aContext.metadata(*date),
        .mType = *benefitType,
        .mName = aCsvRow["name"].get<std::string>(),
        .mIsin = isin.empty() ? std::nullopt : std::optional<Isin>{isin},
        .mAssetClass = parseAssetClass(aParsedValues.mAssetClass),
        .mAmount = *amount,
        .mCurrency = currency,
    });
}

void TradeRepublicParser::parsePrivateMarketRow(
    const csv::CSVRow& aCsvRow,
    std::vector<PrivateMarketEvent>& aPrivateMarketEvents,
    const RowParsedValues& aParsedValues,
    const RowContext& aContext) {
    const auto date = parseCalendarDate(aCsvRow["date"].get<std::string>());
    const auto eventType = parsePrivateMarketEventType(aParsedValues.mType);
    const auto amount = parseMoney(aCsvRow["amount"].get<std::string>());
    const auto feePaid = parseFeePaid(aCsvRow["fee"].get<std::string>());
    const auto currency = parseCurrency(aCsvRow["currency"].get<std::string>());

    if (!date)
    {
        aContext.addInvalidFieldDiagnostic("date",
                                           aCsvRow["date"].get<std::string>(),
                                           "private-market row");
        return;
    }

    if (!eventType)
    {
        aContext.addInvalidFieldDiagnostic("type", aParsedValues.mType, "private-market row");
        return;
    }

    if (!amount)
    {
        aContext.addInvalidFieldDiagnostic("amount",
                                           aCsvRow["amount"].get<std::string>(),
                                           "private-market row");
        return;
    }

    if (!feePaid)
    {
        aContext.addInvalidFieldDiagnostic("fee",
                                           aCsvRow["fee"].get<std::string>(),
                                           "private-market row");
        return;
    }

    if (currency == Currency::Unknown)
    {
        aContext.addInvalidFieldDiagnostic("currency",
                                           aCsvRow["currency"].get<std::string>(),
                                           "private-market row");
        return;
    }

    const auto isin = aCsvRow["symbol"].get<std::string>();
    aPrivateMarketEvents.emplace_back(PrivateMarketEvent{
        .mMetadata = aContext.metadata(*date),
        .mType = *eventType,
        .mName = aCsvRow["name"].get<std::string>(),
        .mIsin = isin.empty() ? std::nullopt : std::optional<Isin>{isin},
        .mAssetClass = parseAssetClass(aParsedValues.mAssetClass),
        .mAmount = *amount,
        .mFeePaid = *feePaid,
        .mCurrency = currency,
        .mDescription = aCsvRow["description"].get<std::string>(),
    });
}

std::optional<Money> TradeRepublicParser::parseMoney(std::string_view aValue) {
    const auto imported = importFixedPoint(aValue, MONEY_SCALE);

    if (const auto* value = std::get_if<ParsedFixedPoint>(&imported))
    {
        return value->mValue;
    }

    return std::nullopt;
}

std::optional<ExchangeRate> TradeRepublicParser::parseExchangeRate(std::string_view aValue) {
    // Preserve broker import rounding; official user input is validated separately.
    const auto imported = importExchangeRate(aValue, true);

    if (const auto* value = std::get_if<ExchangeRate>(&imported))
    {
        return *value;
    }

    return std::nullopt;
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
        return std::nullopt;
    }

    const auto imported = importFixedPoint(aValue, MONEY_SCALE);
    const auto* value = std::get_if<ParsedFixedPoint>(&imported);
    const auto signedTax = value ? std::optional<Money>{value->mValue} : std::nullopt;

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

    const auto imported = importFixedPoint(aValue, MONEY_SCALE);
    const auto* value = std::get_if<ParsedFixedPoint>(&imported);
    const auto signedFee = value ? std::optional<Money>{value->mValue} : std::nullopt;

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
        return BenefitType::StockPerk;
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
