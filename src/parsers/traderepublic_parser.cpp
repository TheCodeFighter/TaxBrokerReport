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
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    if (instrumentIt->mName.empty())
    {
        instrumentIt->mName = aName;
    }

    return *instrumentIt;
}

std::string getAmountCurrencyValue(const csv::CSVRow& aCsvRow) {
    const auto originalCurrency = aCsvRow["original_currency"].get<std::string>();
    return originalCurrency.empty() ? aCsvRow["currency"].get<std::string>() : originalCurrency;
}

/// Number of rows with a comma or parenthesis number from which the file-level hint is added.
constexpr std::size_t LOCALIZED_NUMBER_HINT_MIN_ROWS = 3;

/// True for the numeric fields that Trade Republic exports as plain dot decimals.
bool isNumericField(std::string_view aField) {
    return aField == "price" || aField == "shares" || aField == "amount" ||
           aField == "original_amount" || aField == "fee" || aField == "tax" || aField == "fx_rate";
}

/// True when a rejected number contains a comma or a parenthesis, as a spreadsheet program writes
/// numbers after saving a file again with local number settings.
bool looksLikeLocalizedNumber(std::string_view aValue) {
    return aValue.find_first_of(",()") != std::string_view::npos;
}

/// Columns the parser reads by name. The other columns of the export are not used.
constexpr std::array<std::string_view, 18> REQUIRED_COLUMNS{
    "datetime",
    "date",
    "category",
    "type",
    "asset_class",
    "name",
    "symbol",
    "shares",
    "price",
    "amount",
    "fee",
    "tax",
    "currency",
    "original_amount",
    "original_currency",
    "fx_rate",
    "description",
    "transaction_id",
};

/// Required column names that the header does not contain, in REQUIRED_COLUMNS order.
std::vector<std::string_view> missingRequiredColumns(const std::vector<std::string>& aHeader) {
    std::vector<std::string_view> missing;

    std::copy_if(REQUIRED_COLUMNS.begin(),
                 REQUIRED_COLUMNS.end(),
                 std::back_inserter(missing),
                 [&aHeader](std::string_view aColumn) {
                     return std::find(aHeader.begin(), aHeader.end(), aColumn) == aHeader.end();
                 });

    return missing;
}

/// True when the whole header is one column that contains a semicolon or a tab, as a
/// spreadsheet program writes a file saved with local settings.
bool hasOtherSeparator(const std::vector<std::string>& aHeader) {
    return aHeader.size() == 1 && aHeader.front().find_first_of(";\t") != std::string::npos;
}

/// Message of the file-level `invalid_header` error. It lists only our own column names.
std::string invalidHeaderMessage(const std::vector<std::string>& aHeader,
                                 const std::vector<std::string_view>& aMissingColumns) {
    if (hasOtherSeparator(aHeader))
    {
        return "The columns are separated by semicolons or tabs, not commas; the file was "
               "probably opened and saved again in a spreadsheet program. Download the export "
               "again from Trade Republic and import it unchanged. No rows were imported.";
    }

    std::string names;

    for (const std::string_view column : aMissingColumns)
    {
        names += names.empty() ? "" : ", ";
        names += column;
    }

    const bool isSingle = aMissingColumns.size() == 1;

    return "The file is not a Trade Republic transaction export: " +
           std::to_string(aMissingColumns.size()) + " required column" + (isSingle ? "" : "s") +
           " " + (isSingle ? "is" : "are") + " missing (" + names + "). No rows were imported.";
}

/// An odd number of quote characters means a quoted field never closes. The CSV reader would
/// then merge the following records into that field without any error, so the file is rejected.
/// An unreadable file returns false; the reader reports that case itself.
bool hasUnclosedQuote(const std::filesystem::path& aCsvPath) {
    std::ifstream input{aCsvPath, std::ios::binary};
    if (!input)
    {
        return false;
    }

    const auto quoteCount =
        std::count(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}, '"');

    return quoteCount % 2 != 0;
}

/// A line break inside a field of a record with the wrong field count means a quote crossed a
/// line end, so the reader merged several rows into this record.
bool hasLineBreakInsideField(const csv::CSVRow& aCsvRow) {
    bool hasLineBreak = false;
    for (const csv::CSVField& field : aCsvRow)
    {
        hasLineBreak =
            hasLineBreak || field.get_sv().find_first_of("\r\n") != std::string_view::npos;
    }

    return hasLineBreak;
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
    mutable bool mRowRejected{};        ///< The row has an error that keeps it from being stored.
    mutable bool mHasLocalizedNumber{}; ///< A rejected numeric field has a comma or parenthesis.

    [[nodiscard]] EventMetadata metadata(Date aTaxDate) const;
    [[nodiscard]] std::vector<InstrumentSourceEvidence>
    instrumentEvidence(const EventMetadata& aMetadata,
                       std::string aName,
                       AssetClass aAssetClass,
                       std::string aLabel) const;
    void preserveAssetClass(TradeInstrument& aInstrument,
                            AssetClass aAssetClass,
                            std::string_view aLabel) const;

    void addDiagnostic(DiagnosticSeverity aSeverity,
                       DiagnosticCode aCode,
                       std::string aMessage,
                       std::optional<std::string> aField = std::nullopt) const;

    /// Adds an error and marks the row as rejected; the row parser stores nothing for it.
    void rejectRow(DiagnosticCode aCode,
                   std::string aMessage,
                   std::optional<std::string> aField = std::nullopt) const;

    void addInvalidFieldDiagnostic(std::string_view aField,
                                   std::string_view aValue,
                                   std::string_view aRowKind) const;
};

/// Preserves the original name and classification with the event source; warns on a blank name.
std::vector<InstrumentSourceEvidence>
TradeRepublicParser::RowContext::instrumentEvidence(const EventMetadata& aMetadata,
                                                    std::string aName,
                                                    AssetClass aAssetClass,
                                                    std::string aLabel) const {
    if (aName.empty())
    {
        addDiagnostic(DiagnosticSeverity::Warning,
                      DiagnosticCode::MissingField,
                      "Display name is missing; the identified event was retained.",
                      "name");
    }

    std::vector<InstrumentSourceEvidence> evidence;

    evidence.push_back(InstrumentSourceEvidence{
        .mSource = primarySource(aMetadata),
        .mName = std::move(aName),
        .mAssetClass = aAssetClass,
        .mAssetClassLabel = std::move(aLabel),
    });

    return evidence;
}

/// Retains valid events while diagnosing missing or conflicting classification.
/// An unknown summary stays unresolved even when a later row supplies a known class.
void TradeRepublicParser::RowContext::preserveAssetClass(TradeInstrument& aInstrument,
                                                         AssetClass aAssetClass,
                                                         std::string_view aLabel) const {
    if (aAssetClass == AssetClass::Unknown)
    {
        addDiagnostic(DiagnosticSeverity::Error,
                      aLabel.empty() ? DiagnosticCode::MissingField
                                     : DiagnosticCode::UnsupportedAssetClass,
                      "Asset classification is unresolved; the event was retained.",
                      "asset_class");
    }

    if (aInstrument.mTransactions.empty() && aInstrument.mCorporateActions.empty())
    {
        aInstrument.mAssetClass = aAssetClass;
    }
    else if (aInstrument.mAssetClass != aAssetClass)
    {
        if (aInstrument.mAssetClass != AssetClass::Unknown && aAssetClass != AssetClass::Unknown)
        {
            addDiagnostic(DiagnosticSeverity::Error,
                          DiagnosticCode::InconsistentValue,
                          "Asset classes differ for the same ISIN; events were retained with "
                          "unresolved classification.",
                          "asset_class");
        }

        aInstrument.mAssetClass = AssetClass::Unknown;
    }
}

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

void TradeRepublicParser::RowContext::rejectRow(DiagnosticCode aCode,
                                                std::string aMessage,
                                                std::optional<std::string> aField) const {
    mRowRejected = true;
    addDiagnostic(DiagnosticSeverity::Error, aCode, std::move(aMessage), std::move(aField));
}

void TradeRepublicParser::RowContext::addInvalidFieldDiagnostic(std::string_view aField,
                                                                std::string_view aValue,
                                                                std::string_view aRowKind) const {
    const bool isMissing = aValue.empty();

    if (isNumericField(aField) && looksLikeLocalizedNumber(aValue))
    {
        mHasLocalizedNumber = true;
    }

    mRowRejected = true;
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
        if (hasUnclosedQuote(aCsvPath))
        {
            LOG_ERROR("Trade Republic CSV source {} has an unclosed quote", aSourceIndex);
            parsedResult.mDiagnostics.emplace_back(ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::ParseError,
                .mSourceFile = sourceFile.value(),
                .mMessage = "The CSV file has an unclosed quote; it may be cut off or edited. "
                            "No rows were imported.",
            });

            return parsedResult;
        }

        // KEEP delivers every record. The default silently drops records whose field count
        // differs from the header, which also shifts all later row numbers.
        csv::CSVFormat format;
        format.delimiter(delimiter).variable_columns(csv::VariableColumnPolicy::KEEP);
        csv::CSVReader reader(aCsvPath.string(), format);
        const auto headerNames = reader.get_col_names();
        const std::size_t headerFieldCount = headerNames.size();
        const auto missingColumns = missingRequiredColumns(headerNames);

        if (!missingColumns.empty())
        {
            LOG_ERROR("Trade Republic CSV source {} has an unexpected header", aSourceIndex);
            parsedResult.mDiagnostics.emplace_back(ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::InvalidHeader,
                .mSourceFile = sourceFile.value(),
                .mMessage = invalidHeaderMessage(headerNames, missingColumns),
            });

            return parsedResult;
        }

        std::size_t rowIndex = 1;
        std::size_t localizedNumberRows = 0;
        for (const csv::CSVRow& row : reader)
        {
            ++rowIndex;

            if (row.size() != headerFieldCount)
            {
                if (hasLineBreakInsideField(row))
                {
                    // Several rows were merged into this record, so later rows and row numbers
                    // cannot be trusted.
                    LOG_ERROR("Trade Republic CSV source {} has merged rows", aSourceIndex);
                    parsedResult.mStatement = {};
                    parsedResult.mDiagnostics.emplace_back(ParseDiagnostic{
                        .mSeverity = DiagnosticSeverity::Error,
                        .mCode = DiagnosticCode::ParseError,
                        .mSourceFile = sourceFile.value(),
                        .mMessage = "A row with the wrong number of fields contains a line break "
                                    "inside a field; a quote is probably unclosed. No rows were "
                                    "imported.",
                    });

                    return parsedResult;
                }

                // Values of a record with the wrong field count may be shifted, so the row is
                // rejected before any named field is read.
                const RowContext mismatchContext{
                    .mParseResult = parsedResult,
                    .mSourceFile = sourceFile,
                    .mRowIndex = rowIndex,
                    .mInputSequence =
                        StableInputSequence{
                            .mSourceIndex = aSourceIndex,
                            .mEventIndex = rowIndex - 2,
                        },
                    .mTransactionId = {},
                    .mSourceTimestamp = std::nullopt,
                    .mSourceCalendarDate = std::nullopt,
                    .mSourceTimestampInvalid = false,
                };

                mismatchContext.addDiagnostic(
                    DiagnosticSeverity::Error,
                    DiagnosticCode::FieldCountMismatch,
                    "The row has a different number of fields than the header (expected " +
                        std::to_string(headerFieldCount) + ", found " + std::to_string(row.size()) +
                        "); the row was skipped.");
                continue;
            }

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
                context.rejectRow(DiagnosticCode::MissingField,
                                  "Required field 'datetime' is missing; transaction order "
                                  "cannot be established. "
                                  "The row was skipped; review the source transaction.",
                                  "datetime");
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

            if (context.mHasLocalizedNumber)
            {
                ++localizedNumberRows;
            }
        }

        if (localizedNumberRows >= LOCALIZED_NUMBER_HINT_MIN_ROWS)
        {
            parsedResult.mDiagnostics.emplace_back(ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Warning,
                .mCode = DiagnosticCode::LocalizedNumberFormat,
                .mSourceFile = sourceFile.value(),
                .mMessage = std::to_string(localizedNumberRows) +
                            " rows have a number with a comma or parentheses, which Trade "
                            "Republic does not export; the file was probably opened and saved "
                            "again in a spreadsheet program. Download the export again from "
                            "Trade Republic and import it unchanged.",
            });
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

void TradeRepublicParser::isInstrumentValid(std::string_view aContext,
                                            const std::string& aIsin,
                                            const std::string& aName,
                                            const RowContext& aRowContext) {
    if (aIsin.empty() && aName.empty())
    {
        aRowContext.rejectRow(DiagnosticCode::MissingField,
                              "Required fields 'symbol' and 'name' are missing in " +
                                  std::string{aContext} + "; the row was skipped.");
        return;
    }

    if (aIsin.empty())
    {
        aRowContext.addInvalidFieldDiagnostic("symbol", aIsin, aContext);
    }
}

GetAmount TradeRepublicParser::getAmountAndCurrency(const csv::CSVRow& aCsvRow) {
    const auto originalCurrencyValue = aCsvRow["original_currency"].get<std::string>();
    const bool usesOriginalCurrency = !originalCurrencyValue.empty();
    const auto currency = parseCurrency(
        usesOriginalCurrency ? originalCurrencyValue : aCsvRow["currency"].get<std::string>());

    // The amount, the currency and the rate are parsed on their own, so that each invalid
    // field can be reported.
    auto grossAmount =
        parseMoney(usesOriginalCurrency ? aCsvRow["original_amount"].get<std::string>()
                                        : aCsvRow["amount"].get<std::string>());
    const auto rateText = aCsvRow["fx_rate"].get<std::string>();
    auto exchangeRate = parseExchangeRate(rateText);

    if (!usesOriginalCurrency && currency == Currency::EUR && rateText.empty())
    {
        exchangeRate = EXCHANGE_RATE_SCALE;
    }

    if (grossAmount && *grossAmount < 0)
    {
        grossAmount.reset();
    }

    return GetAmount{
        .mGrossAmount = grossAmount,
        .mExchangeRate = exchangeRate,
        .mCurrency = currency == Currency::Unknown ? std::nullopt : std::optional{currency},
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
    const auto& classLabel = aParsedValues.mAssetClass;
    const auto assetClass = parseAssetClass(classLabel);

    // Every invalid field is reported, in the column order of the export.
    if (!date)
    {
        aContext.addInvalidFieldDiagnostic("date", aCsvRow["date"].get<std::string>(), "trade row");
    }

    if (!tradeSide)
    {
        aContext.addInvalidFieldDiagnostic("type", typeValue, "trade row");
    }

    isInstrumentValid("trade row", isinValue, nameValue, aContext);

    if (!units)
    {
        aContext.addInvalidFieldDiagnostic("shares",
                                           aCsvRow["shares"].get<std::string>(),
                                           "trade row");
    }

    if (!unitPrice || *unitPrice <= 0)
    {
        aContext.addInvalidFieldDiagnostic("price",
                                           aCsvRow["price"].get<std::string>(),
                                           "trade row");
    }

    if (!amountValue.empty() && !amount)
    {
        aContext.addInvalidFieldDiagnostic("amount", amountValue, "trade row");
    }

    if (!feePaid)
    {
        aContext.addInvalidFieldDiagnostic("fee", aCsvRow["fee"].get<std::string>(), "trade row");
    }

    if (currency == Currency::Unknown)
    {
        aContext.addInvalidFieldDiagnostic("currency",
                                           aCsvRow["currency"].get<std::string>(),
                                           "trade row");
    }

    // The sign checks use only fields that are valid. Normalize stored trade units; evidence
    // retains the original source sign.
    std::optional<Units> normalizedUnits;

    if (tradeSide && units)
    {
        normalizedUnits = normalizeTradeUnits(*tradeSide, units->mValue);
        if (!normalizedUnits)
        {
            aContext.rejectRow(DiagnosticCode::InconsistentValue,
                               "Field 'shares' is inconsistent with the trade side; the row was "
                               "skipped.",
                               "shares");
        }
    }

    if (tradeSide && amount)
    {
        const auto normalizedAmount = normalizeTradeAmount(*tradeSide, *amount);
        if (!normalizedAmount)
        {
            aContext.rejectRow(DiagnosticCode::InconsistentValue,
                               "Field 'amount' is inconsistent with the trade side; the row was "
                               "skipped.",
                               "amount");
        }
        amount = normalizedAmount;
    }

    if (aContext.mRowRejected)
    {
        return false;
    }

    auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);

    aContext.preserveAssetClass(instrument, assetClass, classLabel);

    auto metadata = aContext.metadata(*date);
    auto evidence = unitSourceEvidence(*units, sharesText, primarySource(metadata));
    auto instrumentEvidence =
        aContext.instrumentEvidence(metadata, nameValue, assetClass, classLabel);

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
        .mInstrumentEvidence = std::move(instrumentEvidence),
    });

    return true;
}

std::optional<TradeRepublicParser::ParsedIncome>
TradeRepublicParser::parseIncome(const csv::CSVRow& aCsvRow,
                                 std::string_view aRowKind,
                                 bool aCheckInstrument,
                                 const RowContext& aContext) {
    const auto dateText = aCsvRow["date"].get<std::string>();
    const auto taxText = aCsvRow["tax"].get<std::string>();
    const auto currencyText = aCsvRow["currency"].get<std::string>();
    const auto rateText = aCsvRow["fx_rate"].get<std::string>();
    const bool usesOriginalCurrency = !aCsvRow["original_currency"].get<std::string>().empty();
    const auto date = parseCalendarDate(dateText);
    const auto taxPaid = parseTaxPaid(taxText);
    const auto taxCurrency = parseCurrency(currencyText);
    const auto amountAndCurrency = getAmountAndCurrency(aCsvRow);

    // Every invalid field is reported, in the column order of the export: amount comes before
    // tax, original_amount after currency.
    if (!date)
    {
        aContext.addInvalidFieldDiagnostic("date", dateText, aRowKind);
    }

    if (aCheckInstrument)
    {
        isInstrumentValid(aRowKind,
                          aCsvRow["symbol"].get<std::string>(),
                          aCsvRow["name"].get<std::string>(),
                          aContext);
    }

    if (!usesOriginalCurrency && !amountAndCurrency.mGrossAmount)
    {
        const auto [fieldName, fieldValue] = pickAmountField(aCsvRow);

        aContext.addInvalidFieldDiagnostic(fieldName, fieldValue, aRowKind);
    }

    if (!taxPaid && !taxText.empty())
    {
        aContext.addInvalidFieldDiagnostic("tax", taxText, aRowKind);
    }

    // Without original_currency, this is also the currency of the amount.
    if (taxCurrency == Currency::Unknown)
    {
        aContext.addInvalidFieldDiagnostic("currency", currencyText, aRowKind);
    }

    if (usesOriginalCurrency && !amountAndCurrency.mGrossAmount)
    {
        const auto [fieldName, fieldValue] = pickAmountField(aCsvRow);

        aContext.addInvalidFieldDiagnostic(fieldName, fieldValue, aRowKind);
    }

    if (usesOriginalCurrency && !amountAndCurrency.mCurrency)
    {
        aContext.addInvalidFieldDiagnostic("original_currency",
                                           getAmountCurrencyValue(aCsvRow),
                                           aRowKind);
    }

    if (!amountAndCurrency.mExchangeRate && !rateText.empty())
    {
        aContext.addInvalidFieldDiagnostic("fx_rate", rateText, aRowKind);
    }

    if (aContext.mRowRejected)
    {
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

    const auto income = parseIncome(aCsvRow, "dividend row", true, aContext);

    if (!income)
    {
        return;
    }

    auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);
    auto metadata = aContext.metadata(income->mDate);
    const auto classLabel = aCsvRow["asset_class"].get<std::string>();
    auto evidence =
        aContext.instrumentEvidence(metadata, nameValue, parseAssetClass(classLabel), classLabel);

    instrument.mTransactions.emplace_back(DividendTransaction{
        .mMetadata = std::move(metadata),
        .mGrossAmount = income->mGrossAmount,
        .mTaxPaid = income->mTaxPaid,
        .mExchangeRate = income->mExchangeRate,
        .mCurrency = income->mCurrency,
        .mTaxCurrency = income->mTaxCurrency,
        .mInstrumentEvidence = std::move(evidence),
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

    const auto income = parseIncome(aCsvRow, rowKind, isBond, aContext);

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

    if (instrument->mName.empty())
    {
        instrument->mName = name;
    }

    auto metadata = aContext.metadata(income->mDate);
    std::vector<InstrumentSourceEvidence> evidence;

    if (isBond)
    {
        evidence = aContext.instrumentEvidence(metadata, name, AssetClass::Bond, "BOND");
    }

    instrument->mTransactions.emplace_back(InterestTransaction{
        .mMetadata = std::move(metadata),
        .mGrossAmount = income->mGrossAmount,
        .mTaxPaid = income->mTaxPaid,
        .mExchangeRate = income->mExchangeRate,
        .mCurrency = income->mCurrency,
        .mTaxCurrency = income->mTaxCurrency,
        .mInstrumentEvidence = std::move(evidence),
    });
}

void TradeRepublicParser::parseCorporateActionRow(const csv::CSVRow& aCsvRow,
                                                  std::vector<TradeInstrument>& aInstruments,
                                                  const RowContext& aContext) {
    const auto isinValue = aCsvRow["symbol"].get<std::string>();
    const auto nameValue = aCsvRow["name"].get<std::string>();

    const auto date = parseCalendarDate(aCsvRow["date"].get<std::string>());
    const auto sharesText = aCsvRow["shares"].get<std::string>();
    const auto importedUnits = importFixedPoint(sharesText, UNITS_SCALE);
    const auto* unitsDelta = std::get_if<ParsedFixedPoint>(&importedUnits);
    const auto classLabel = aCsvRow["asset_class"].get<std::string>();
    const auto assetClass = parseAssetClass(classLabel);

    // Every invalid field is reported, in the column order of the export.
    if (!date)
    {
        aContext.addInvalidFieldDiagnostic("date",
                                           aCsvRow["date"].get<std::string>(),
                                           "corporate-action row");
    }

    isInstrumentValid("corporate-action row", isinValue, nameValue, aContext);

    if (!unitsDelta || unitsDelta->mValue == 0)
    {
        aContext.addInvalidFieldDiagnostic("shares",
                                           aCsvRow["shares"].get<std::string>(),
                                           "corporate-action row");
    }

    if (aContext.mRowRejected)
    {
        return;
    }

    auto& instrument = getOrCreateInstrument(aInstruments, isinValue, nameValue);

    aContext.preserveAssetClass(instrument, assetClass, classLabel);

    // TR shares has no verified economic meaning; only a confirmed ratio establishes direction.
    auto metadata = aContext.metadata(*date);
    auto evidence = unitSourceEvidence(*unitsDelta, sharesText, primarySource(metadata));
    auto instrumentEvidence =
        aContext.instrumentEvidence(metadata, nameValue, assetClass, classLabel);

    instrument.mCorporateActions.emplace_back(CorporateAction{
        .mMetadata = std::move(metadata),
        .mType = CorporateActionType::UnresolvedSplit,
        .mUnitsDelta = unitsDelta->mValue,
        .mRatio = std::nullopt,
        .mUnitEvidence = {std::move(evidence)},
        .mInstrumentEvidence = std::move(instrumentEvidence),
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

    // Every invalid field is reported, in the column order of the export.
    if (!date)
    {
        aContext.addInvalidFieldDiagnostic("date",
                                           aCsvRow["date"].get<std::string>(),
                                           "benefit row");
    }

    if (!benefitType)
    {
        aContext.addInvalidFieldDiagnostic("type", aParsedValues.mType, "benefit row");
    }

    if (!amount)
    {
        aContext.addInvalidFieldDiagnostic("amount",
                                           aCsvRow["amount"].get<std::string>(),
                                           "benefit row");
    }

    if (currency == Currency::Unknown)
    {
        aContext.addInvalidFieldDiagnostic("currency",
                                           aCsvRow["currency"].get<std::string>(),
                                           "benefit row");
    }

    if (aContext.mRowRejected)
    {
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

    // Every invalid field is reported, in the column order of the export.
    if (!date)
    {
        aContext.addInvalidFieldDiagnostic("date",
                                           aCsvRow["date"].get<std::string>(),
                                           "private-market row");
    }

    if (!eventType)
    {
        aContext.addInvalidFieldDiagnostic("type", aParsedValues.mType, "private-market row");
    }

    if (!amount)
    {
        aContext.addInvalidFieldDiagnostic("amount",
                                           aCsvRow["amount"].get<std::string>(),
                                           "private-market row");
    }

    if (!feePaid)
    {
        aContext.addInvalidFieldDiagnostic("fee",
                                           aCsvRow["fee"].get<std::string>(),
                                           "private-market row");
    }

    if (currency == Currency::Unknown)
    {
        aContext.addInvalidFieldDiagnostic("currency",
                                           aCsvRow["currency"].get<std::string>(),
                                           "private-market row");
    }

    if (aContext.mRowRejected)
    {
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
