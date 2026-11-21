#include "parsers/ibkr_parser.hpp"

#include "parsers/ibkr_time.hpp"
#include "taxbroker/currency.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/input_file.hpp"
#include "taxbroker/logging.hpp"
#include "taxbroker/text_format.hpp"
#include "utils/numeric_util.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace taxbroker::ibkr {

namespace {

/// Attributes every `Trade` row needs to be classified. The others are checked once the class is
/// known, because a derivative or a currency conversion is not read any further.
constexpr std::array<const char*, 3> kTradeBaseAttributes{
    "levelOfDetail",
    "transactionID",
    "assetCategory",
};

/// Attributes of a `Trade` row that becomes a trade (`isin` is checked on its own).
constexpr std::array<const char*, 14> kTradeAttributes{
    "transactionType",
    "subCategory",
    "currency",
    "tradeDate",
    "dateTime",
    "buySell",
    "quantity",
    "tradePrice",
    "proceeds",
    "ibCommission",
    "ibCommissionCurrency",
    "taxes",
    "notes",
    "multiplier",
};

/// Attributes every `CashTransaction` and `CorporateAction` row needs to be told apart. The rest
/// is checked once the type is known, because an ignored row is not read any further.
constexpr std::array<const char*, 3> kRowBaseAttributes{
    "transactionID",
    "type",
    "assetCategory",
};

/// Attributes of a cash row that becomes an income event (`isin` and the payment attributes are
/// checked on their own).
constexpr std::array<const char*, 3> kCashAttributes{
    "currency",
    "dateTime",
    "amount",
};

/// Attributes of a corporate action that is read (`proceeds` is checked for a maturity).
constexpr std::array<const char*, 4> kCorporateActionAttributes{
    "isin",
    "currency",
    "dateTime",
    "quantity",
};

/// Asset categories of derivatives and leveraged products (contract rule 6).
constexpr std::array<std::string_view, 9> kDerivativeCategories{
    "OPT",
    "FUT",
    "FOP",
    "FSFOP",
    "FSOPT",
    "CFD",
    "FXCFD",
    "WAR",
    "IOPT",
};

/// Longest canonical decimal read as an exact value: far below what 256 bits can hold.
constexpr std::size_t kMaxExactDecimalLength = 60;

/// Largest difference between |price x units| and |amount| that is still rounding: EUR 0.01.
constexpr Money kTradeAmountTolerance = 100;

/// The attribute text; empty for an attribute that is not there.
std::string_view textOf(const pugi::xml_node& aRow, const char* aName) {
    return aRow.attribute(aName).value();
}

/// The first attribute of the list that the row does not have at all, or none.
const char* firstMissingAttribute(const pugi::xml_node& aRow,
                                  const std::span<const char* const> aNames) {
    const auto missing = std::find_if(aNames.begin(), aNames.end(), [&aRow](const char* aName) {
        return aRow.attribute(aName).empty();
    });

    return missing == aNames.end() ? nullptr : *missing;
}

/// The model class of an asset category (contract rule 6).
AssetClass assetClassOf(const std::string_view aCategory, const std::string_view aSubCategory) {
    if (aCategory == "STK")
    {
        return aSubCategory == "ETF" ? AssetClass::Fund : AssetClass::Stock;
    }

    if (aCategory == "FUND")
    {
        return AssetClass::Fund;
    }

    if (aCategory == "BOND" || aCategory == "BILL")
    {
        return AssetClass::Bond;
    }

    if (std::find(kDerivativeCategories.begin(), kDerivativeCategories.end(), aCategory) !=
        kDerivativeCategories.end())
    {
        return AssetClass::Derivative;
    }

    if (aCategory == "CRYPTO")
    {
        return AssetClass::Crypto;
    }

    return AssetClass::Unknown;
}

/// The first eight characters of a date or date-and-time attribute, which is the day it names.
std::optional<Date> dayOf(const std::string_view aText) {
    return parseCompactDate(aText.substr(0, 8));
}

std::optional<ParsedFixedPoint> readFixedPoint(const std::string_view aText,
                                               const std::int64_t aScale) {
    const auto imported = importFixedPoint(aText, aScale);

    if (const auto* value = std::get_if<ParsedFixedPoint>(&imported))
    {
        return *value;
    }

    return std::nullopt;
}

std::optional<Money> readMoney(const std::string_view aText) {
    const auto value = readFixedPoint(aText, MONEY_SCALE);

    if (!value || value->mValue == std::numeric_limits<Money>::min())
    {
        return std::nullopt;
    }

    return value->mValue;
}

/// The exact value of a plain decimal that the number import accepted.
std::optional<ExactRational> exactDecimal(const ParsedFixedPoint& aParsed) {
    std::string_view text = aParsed.mCanonicalValue;

    if (text.empty() || text.size() > kMaxExactDecimalLength)
    {
        return std::nullopt;
    }

    const bool isNegative = text.front() == '-';

    if (isNegative)
    {
        text.remove_prefix(1);
    }

    const auto point = text.find('.');
    std::string digits{text.substr(0, point)};
    std::size_t decimals = 0;

    if (point != std::string_view::npos)
    {
        digits += text.substr(point + 1);
        decimals = text.size() - point - 1;
    }

    // A leading zero would make the text an octal number.
    const auto firstDigit = digits.find_first_not_of('0');
    digits = firstDigit == std::string::npos ? "0" : digits.substr(firstDigit);

    WideInteger numerator{digits};
    WideInteger denominator{1};

    for (std::size_t digit = 0; digit < decimals; ++digit)
    {
        denominator *= 10;
    }

    const auto exact = ExactRational::create(isNegative ? -numerator : numerator, denominator);

    if (const auto* value = std::get_if<ExactRational>(&exact))
    {
        return *value;
    }

    return std::nullopt;
}

/// True for a value that is a number greater than zero in the source text.
bool isPositive(const ParsedFixedPoint& aValue) {
    return !aValue.mSourceNegative && aValue.mSourceNonzero;
}

/// The unit price: `tradePrice` times `multiplier`, rounded once to eight decimals. A bond at
/// 99.5 with the multiplier 0.01 is 0.995 per unit of nominal. Both values must be positive. None
/// when the product does not fit or is zero at eight decimals.
std::optional<UnitPrice> unitPriceOf(const ParsedFixedPoint& aPrice,
                                     const ParsedFixedPoint& aMultiplier) {
    const auto price = exactDecimal(aPrice);
    const auto multiplier = exactDecimal(aMultiplier);

    if (!price || !multiplier)
    {
        return std::nullopt;
    }

    const auto product = multiplyExact(*price, *multiplier);

    if (const auto* exact = std::get_if<ExactRational>(&product))
    {
        const auto rounded = roundToScaled(*exact,
                                           UNIT_PRICE_SCALE,
                                           DestinationRange{.mPolicy = ValuePolicy::Positive});

        if (const auto* value = std::get_if<std::int64_t>(&rounded))
        {
            return *value;
        }
    }

    return std::nullopt;
}

/// True when |amount| differs from |unit price x units| by more than the tolerance. The
/// comparison is exact, at the scale of price times units, with no rounding.
bool exceedsTradeAmountTolerance(const UnitPrice aUnitPrice,
                                 const Units aUnits,
                                 const Money aAmount) {
    constexpr std::int64_t kMoneyToProductScale = (UNIT_PRICE_SCALE * UNITS_SCALE) / MONEY_SCALE;

    const WideInteger product = abs(WideInteger{aUnitPrice} * aUnits);
    const WideInteger amount = abs(WideInteger{aAmount}) * kMoneyToProductScale;
    const WideInteger tolerance = WideInteger{kTradeAmountTolerance} * kMoneyToProductScale;

    return abs(product - amount) > tolerance;
}

/// True when the `notes` codes (joined by `;`) include `Ca`, the code of a cancelled trade.
bool notesHaveCancelCode(std::string_view aNotes) {
    while (!aNotes.empty())
    {
        const auto separator = aNotes.find(';');
        const auto code = aNotes.substr(0, separator);

        if (code == "Ca")
        {
            return true;
        }

        if (separator == std::string_view::npos)
        {
            break;
        }

        aNotes.remove_prefix(separator + 1);
    }

    return false;
}

/// The attribute that shows a trade row is cancelled or corrected (contract rule 7), or none.
const char* cancellationAttribute(const pugi::xml_node& aRow) {
    const auto buySell = textOf(aRow, "buySell");
    const auto type = textOf(aRow, "transactionType");

    if (buySell.ends_with("(Ca.)"))
    {
        return "buySell";
    }

    if (type == "TradeCancel" || type == "TradeCorrect")
    {
        return "transactionType";
    }

    return notesHaveCancelCode(textOf(aRow, "notes")) ? "notes" : nullptr;
}

/// The term of a row kind, for the sentence "... in a trade row". A cash row whose kind is not
/// known yet (its `type` is missing) is named as a cash row. The kinds this parser never makes
/// fall back to it too.
std::string_view termOf(const std::optional<ParsedRowKind> aKind) {
    if (!aKind)
    {
        return term::kRowKindCash;
    }

    switch (*aKind)
    {
    case ParsedRowKind::Trade:
        return term::kRowKindTrade;
    case ParsedRowKind::CorporateAction:
        return term::kRowKindCorporateAction;
    case ParsedRowKind::BondInterest:
        return term::kRowKindBondInterest;
    case ParsedRowKind::BrokerInterest:
        return term::kRowKindBrokerInterest;
    case ParsedRowKind::BondRedemption:
        return term::kRowKindBondRedemption;
    case ParsedRowKind::Dividend:
        return term::kRowKindDividend;
    case ParsedRowKind::IncomeTax:
        return term::kRowKindIncomeTax;
    case ParsedRowKind::UnknownInterest:
    case ParsedRowKind::Benefit:
    case ParsedRowKind::PrivateMarket:
        break;
    }

    return term::kRowKindCash;
}

/// A file-level diagnostic: it has a source file but no row, transaction ID or field.
ParseDiagnostic fileDiagnostic(const DiagnosticSeverity aSeverity,
                               const DiagnosticCode aCode,
                               const SourceFilename& aSourceFile,
                               DiagnosticMessage aMessage) {
    return ParseDiagnostic{
        .mSeverity = aSeverity,
        .mCode = aCode,
        .mSourceFile = aSourceFile.value(),
        .mMessage = std::move(aMessage.mEnglish),
        .mMessageKey = aMessage.mKey,
        .mMessageParams = std::move(aMessage.mParams),
    };
}

template <typename InstrumentT>
InstrumentT& getOrCreateInstrument(std::vector<InstrumentT>& aInstruments,
                                   const std::string& aIsin,
                                   const std::string& aName) {
    auto instrument =
        std::find_if(aInstruments.begin(), aInstruments.end(), [&](const InstrumentT& aCandidate) {
            return aCandidate.mIsin == aIsin;
        });

    if (instrument == aInstruments.end())
    {
        aInstruments.emplace_back(InstrumentT{
            .mName = aName,
            .mIsin = aIsin,
        });
        instrument = std::prev(aInstruments.end());
    }

    if (instrument->mName.empty())
    {
        instrument->mName = aName;
    }

    return *instrument;
}

/// One row element being read: where it is, and the diagnostics it gets.
struct RowContext {
    ParseResult& mResult;
    const SourceFilename& mFile;
    std::size_t mLine{}; ///< Line of the start tag; 0 when it is not known.
    StableInputSequence mSequence;
    std::string_view mTransactionId;
    std::optional<ParsedRowKind> mRowKind; ///< Set once the row type is known.

    [[nodiscard]] SourceReference source() const {
        return SourceReference{
            .mBroker = Broker::InteractiveBrokers,
            .mFilename = mFile,
            .mSourceRow = mLine,
            .mTransactionId =
                mTransactionId.empty() ? std::nullopt : std::optional<std::string>{mTransactionId},
            .mInputSequence = mSequence,
        };
    }

    [[nodiscard]] EventMetadata metadata(const Date aTaxDate,
                                         const std::optional<SourceTimestamp> aTimestamp) const {
        return EventMetadata{
            .mTaxDate = aTaxDate,
            .mSourceTimestamp = aTimestamp,
            .mOrderingTimestamp = std::nullopt,
            .mSources = {source()},
        };
    }

    void diagnose(const DiagnosticSeverity aSeverity,
                  const DiagnosticCode aCode,
                  DiagnosticMessage aMessage,
                  std::optional<std::string> aField = std::nullopt) const {
        // The stable message key, never the English sentence: a sentence can hold a cell text.
        std::string messageKey{messageDefinition(aMessage.mKey).mId};

        if (aField)
        {
            messageKey += " (" + *aField + ")";
        }

        switch (aSeverity)
        {
        case DiagnosticSeverity::Info:
            LOG_INFO("Interactive Brokers XML source {} line {}: {}",
                     mSequence.mSourceIndex,
                     mLine,
                     messageKey);
            break;
        case DiagnosticSeverity::Warning:
            LOG_WARNING("Interactive Brokers XML source {} line {}: {}",
                        mSequence.mSourceIndex,
                        mLine,
                        messageKey);
            break;
        case DiagnosticSeverity::Error:
            LOG_ERROR("Interactive Brokers XML source {} line {}: {}",
                      mSequence.mSourceIndex,
                      mLine,
                      messageKey);
            break;
        }

        std::optional<std::string> storedTransactionId;

        if (!mTransactionId.empty())
        {
            storedTransactionId.emplace(mTransactionId);
        }

        mResult.mDiagnostics.emplace_back(ParseDiagnostic{
            .mSeverity = aSeverity,
            .mCode = aCode,
            .mSourceFile = mFile.value(),
            .mRowIndex = mLine,
            .mTransactionId = std::move(storedTransactionId),
            .mField = std::move(aField),
            .mMessage = std::move(aMessage.mEnglish),
            .mMessageKey = aMessage.mKey,
            .mMessageParams = std::move(aMessage.mParams),
            .mRowKind = mRowKind,
        });
    }

    /// The error of a row that is not read. It always returns none, so a reader can write
    /// `return aContext.rejectField(...)`.
    std::nullopt_t reject(const DiagnosticCode aCode,
                          DiagnosticMessage aMessage,
                          std::optional<std::string> aField = std::nullopt) const {
        diagnose(DiagnosticSeverity::Error, aCode, std::move(aMessage), std::move(aField));

        return std::nullopt;
    }

    /// A field that is empty (a missing field) or not a valid value.
    std::nullopt_t rejectField(const std::string_view aField, const std::string_view aValue) const {
        const bool isEmpty = aValue.empty();

        return reject(
            isEmpty ? DiagnosticCode::MissingField : DiagnosticCode::InvalidValue,
            makeDiagnosticMessage(
                isEmpty ? MessageKey::MissingFieldRequiredField : MessageKey::InvalidValueField,
                {textParam("field", std::string{aField}), termParam("rowKind", termOf(mRowKind))}),
            std::string{aField});
    }

    /// Gives the missing-field error and returns true when the row has no transaction ID. The
    /// merger drops the repeat of a row in an overlapping file only by its ID, so a row without
    /// one is not read.
    [[nodiscard]] bool rejectEmptyTransactionId() const {
        if (!mTransactionId.empty())
        {
            return false;
        }

        rejectField("transactionID", "");

        return true;
    }

    /// An attribute that the row does not have at all.
    std::nullopt_t rejectMissingAttribute(const std::string_view aField) const {
        return reject(DiagnosticCode::InvalidHeader,
                      makeDiagnosticMessage(MessageKey::InvalidHeaderMissingAttribute,
                                            {textParam("field", std::string{aField}),
                                             termParam("rowKind", termOf(mRowKind))}),
                      std::string{aField});
    }

    /// A sign that disagrees with the side of the trade.
    std::nullopt_t rejectSign(const std::string_view aField) const {
        return reject(DiagnosticCode::InconsistentValue,
                      makeDiagnosticMessage(MessageKey::InconsistentValueTradeSide,
                                            {textParam("field", std::string{aField})}),
                      std::string{aField});
    }

    /// A row that is not read on purpose and belongs to no form: it only leaves this notice.
    void informNoForm(const DiagnosticCode aCode, DiagnosticMessage aMessage) {
        mRowKind.reset();
        diagnose(DiagnosticSeverity::Info, aCode, std::move(aMessage));
    }

    /// Evidence of the original name and class of a row; warns when the name is blank.
    [[nodiscard]] std::vector<InstrumentSourceEvidence>
    instrumentEvidence(std::string aName, const AssetClass aAssetClass, std::string aLabel) const {
        if (aName.empty())
        {
            diagnose(DiagnosticSeverity::Warning,
                     DiagnosticCode::MissingField,
                     makeDiagnosticMessage(MessageKey::MissingFieldName),
                     "description");
        }

        std::vector<InstrumentSourceEvidence> evidence;

        evidence.push_back(InstrumentSourceEvidence{
            .mSource = source(),
            .mName = std::move(aName),
            .mAssetClass = aAssetClass,
            .mAssetClassLabel = std::move(aLabel),
        });

        return evidence;
    }

    /// Keeps the instrument's class, and reports a missing, unknown or conflicting class. An
    /// unknown class stays unresolved even when a later row names a known one.
    void preserveAssetClass(TradeInstrument& aInstrument,
                            const AssetClass aAssetClass,
                            const std::string_view aLabel) const {
        if (aAssetClass == AssetClass::Unknown)
        {
            diagnose(DiagnosticSeverity::Error,
                     aLabel.empty() ? DiagnosticCode::MissingField
                                    : DiagnosticCode::UnsupportedAssetClass,
                     makeDiagnosticMessage(aLabel.empty()
                                               ? MessageKey::MissingFieldAssetClass
                                               : MessageKey::UnsupportedAssetClassUnrecognized),
                     "assetCategory");
        }

        // Only trades name a class: a corporate action row has no `subCategory`, so it cannot tell
        // a fund from a share.
        if (aInstrument.mTransactions.empty())
        {
            aInstrument.mAssetClass = aAssetClass;
        }
        else if (aInstrument.mAssetClass != aAssetClass)
        {
            if (aInstrument.mAssetClass != AssetClass::Unknown &&
                aAssetClass != AssetClass::Unknown)
            {
                diagnose(DiagnosticSeverity::Error,
                         DiagnosticCode::InconsistentValue,
                         makeDiagnosticMessage(MessageKey::InconsistentValueAssetClass),
                         "assetCategory");
            }

            aInstrument.mAssetClass = AssetClass::Unknown;
        }
    }
};

/// The fields of a trade row that passed every check.
struct TradeFields {
    Date mTaxDate;
    std::optional<SourceTimestamp> mTimestamp;
    TradeSide mSide{};
    ParsedFixedPoint mQuantity;
    Units mUnits{};
    UnitPrice mUnitPrice{};
    Money mAmount{};
    Money mFee{};
    Money mTransactionTax{};
    Currency mCurrency{};
    Currency mFeeCurrency{};
};

/// A fee or tax attribute: a blank one is zero, a value must be zero or negative (it is a cost).
/// Returns the positive amount, or none for a value that is not a valid amount.
std::optional<Money> readCost(const std::string_view aText, bool& aHasWrongSign) {
    if (aText.empty())
    {
        return Money{0};
    }

    const auto value = readMoney(aText);

    if (!value)
    {
        return std::nullopt;
    }

    aHasWrongSign = *value > 0;

    return std::abs(*value);
}

/// The side of a trade from `buySell`; none for any other text.
std::optional<TradeSide> readSide(const std::string_view aText) {
    if (aText == "BUY")
    {
        return TradeSide::Buy;
    }

    if (aText == "SELL")
    {
        return TradeSide::Sell;
    }

    return std::nullopt;
}

/// Reads and checks the fields of a trade row. The row gets the error of the first check that
/// fails, in this order: format of each field, then the signs.
std::optional<TradeFields> readTradeFields(const pugi::xml_node& aRow, const RowContext& aContext) {
    if (aContext.rejectEmptyTransactionId())
    {
        return std::nullopt;
    }

    if (textOf(aRow, "isin").empty())
    {
        return aContext.rejectField("isin", "");
    }

    const auto tradeDate = parseCompactDate(textOf(aRow, "tradeDate"));

    if (!tradeDate)
    {
        return aContext.rejectField("tradeDate", textOf(aRow, "tradeDate"));
    }

    const auto dateTime = parseCompactDateTime(textOf(aRow, "dateTime"), TimeOfDay::Required);

    if (!dateTime)
    {
        return aContext.rejectField("dateTime", textOf(aRow, "dateTime"));
    }

    const auto currency = parseCurrencyCode(textOf(aRow, "currency"));

    if (currency == Currency::Unknown)
    {
        return aContext.rejectField("currency", textOf(aRow, "currency"));
    }

    const auto feeCurrency = parseCurrencyCode(textOf(aRow, "ibCommissionCurrency"));

    if (feeCurrency == Currency::Unknown)
    {
        return aContext.rejectField("ibCommissionCurrency", textOf(aRow, "ibCommissionCurrency"));
    }

    const auto side = readSide(textOf(aRow, "buySell"));

    if (!side)
    {
        return aContext.rejectField("buySell", textOf(aRow, "buySell"));
    }

    const auto quantity = readFixedPoint(textOf(aRow, "quantity"), UNITS_SCALE);

    if (!quantity)
    {
        return aContext.rejectField("quantity", textOf(aRow, "quantity"));
    }

    const auto price = readFixedPoint(textOf(aRow, "tradePrice"), UNIT_PRICE_SCALE);

    if (!price || !isPositive(*price))
    {
        return aContext.rejectField("tradePrice", textOf(aRow, "tradePrice"));
    }

    // The multiplier has its own check: a bond price is a percentage of the nominal, and a bad
    // multiplier must not be reported as a bad price.
    const auto multiplier = readFixedPoint(textOf(aRow, "multiplier"), UNIT_PRICE_SCALE);

    if (!multiplier || !isPositive(*multiplier))
    {
        return aContext.rejectField("multiplier", textOf(aRow, "multiplier"));
    }

    const auto unitPrice = unitPriceOf(*price, *multiplier);

    if (!unitPrice)
    {
        return aContext.rejectField("tradePrice", textOf(aRow, "tradePrice"));
    }

    const auto proceeds = readMoney(textOf(aRow, "proceeds"));

    if (!proceeds)
    {
        return aContext.rejectField("proceeds", textOf(aRow, "proceeds"));
    }

    bool isCommissionPositive = false;
    bool isTaxPositive = false;
    const auto fee = readCost(textOf(aRow, "ibCommission"), isCommissionPositive);

    if (!fee)
    {
        return aContext.rejectField("ibCommission", textOf(aRow, "ibCommission"));
    }

    const auto tax = readCost(textOf(aRow, "taxes"), isTaxPositive);

    if (!tax)
    {
        return aContext.rejectField("taxes", textOf(aRow, "taxes"));
    }

    // Signs: a buy has positive units and negative proceeds, a sell the other way round, and a
    // fee or tax is a cost. The stored values are all positive.
    const bool isBuy = *side == TradeSide::Buy;
    const bool isQuantityRight = isBuy ? quantity->mValue > 0 : quantity->mValue < 0;
    const bool isProceedsRight = isBuy ? *proceeds < 0 : *proceeds > 0;

    if (!isQuantityRight || quantity->mValue == std::numeric_limits<Units>::min())
    {
        return aContext.rejectSign("quantity");
    }

    if (!isProceedsRight)
    {
        return aContext.rejectSign("proceeds");
    }

    if (isCommissionPositive)
    {
        return aContext.rejectSign("ibCommission");
    }

    if (isTaxPositive)
    {
        return aContext.rejectSign("taxes");
    }

    return TradeFields{
        .mTaxDate = *tradeDate,
        .mTimestamp = dateTime->mTimestamp,
        .mSide = *side,
        .mQuantity = *quantity,
        .mUnits = std::abs(quantity->mValue),
        .mUnitPrice = *unitPrice,
        .mAmount = std::abs(*proceeds),
        .mFee = *fee,
        .mTransactionTax = *tax,
        .mCurrency = currency,
        .mFeeCurrency = feeCurrency,
    };
}

/// What a `CashTransaction` row is, from its `type` (contract rule 9).
enum class CashType {
    Dividend,
    WithholdingTax,
    PaymentInLieu,
    BrokerInterestReceived,
    BrokerInterestPaid,
    BondInterestReceived,
    Ignored,
    Unknown,
};

CashType cashTypeOf(const std::string_view aType) {
    if (aType == "Dividends")
    {
        return CashType::Dividend;
    }

    if (aType == "Withholding Tax")
    {
        return CashType::WithholdingTax;
    }

    if (aType == "Payment In Lieu Of Dividends")
    {
        return CashType::PaymentInLieu;
    }

    if (aType == "Broker Interest Received")
    {
        return CashType::BrokerInterestReceived;
    }

    if (aType == "Broker Interest Paid")
    {
        return CashType::BrokerInterestPaid;
    }

    if (aType == "Bond Interest Received")
    {
        return CashType::BondInterestReceived;
    }

    // Accrued interest paid is only a warning of the coupon stage; the others are not income.
    if (aType == "Bond Interest Paid" || aType == "Other Fees" ||
        aType == "Commission Adjustments" || aType == "Advisor Fees" ||
        aType == "Deposits/Withdrawals")
    {
        return CashType::Ignored;
    }

    return CashType::Unknown;
}

/// What a `CorporateAction` row is, from its `type` (contract rule 10).
enum class ActionType {
    Split,       ///< `FS`, `RS`.
    Maturity,    ///< `BM`, `TM`.
    CashPayment, ///< `CD`, `CP`: the cash row is the payment.
    Unsupported,
};

ActionType actionTypeOf(const std::string_view aType) {
    if (aType == "FS" || aType == "RS")
    {
        return ActionType::Split;
    }

    if (aType == "BM" || aType == "TM")
    {
        return ActionType::Maturity;
    }

    if (aType == "CD" || aType == "CP")
    {
        return ActionType::CashPayment;
    }

    return ActionType::Unsupported;
}

/// Reads the whole file: the statements of every account and their rows.
class FlexReader {
  public:
    FlexReader(ParseResult& aResult,
               const SourceFilename& aFile,
               const std::size_t aSourceIndex,
               const std::string_view aText)
        : mResult{aResult}, mFile{aFile}, mSourceIndex{aSourceIndex} {
        mLineStarts.reserve(static_cast<std::size_t>(std::count(aText.begin(), aText.end(), '\n')) +
                            1);
        mLineStarts.push_back(0);

        for (std::size_t position = 0; position < aText.size(); ++position)
        {
            if (aText[position] == '\n')
            {
                mLineStarts.push_back(position + 1);
            }
        }
    }

    /// Reads one account's statement: its entity first, then its rows in document order.
    void readStatement(const pugi::xml_node& aStatement) {
        mEntityCode = textOf(aStatement.child("AccountInformation"), "ibEntity");
        ++mStatements;

        // The entity code is a payer code of the broker list, not an account number.
        LOG_INFO("Interactive Brokers XML source {}: statement {}, entity {}",
                 mSourceIndex,
                 mStatements,
                 mEntityCode.empty() ? "not named" : mEntityCode);

        for (const pugi::xml_node section : aStatement.children())
        {
            const std::string_view name = section.name();

            if (name == "Trades")
            {
                readTrades(section);
            }
            else if (name == "CorporateActions")
            {
                readCorporateActions(section);
            }
            else if (name == "CashTransactions")
            {
                readCashTransactions(section);
            }
        }
    }

    /// Adds the one file-level notice for the rows of the trades section that are not executions,
    /// and logs what the file gave. Only counts are logged, never a value.
    void finish() {
        if (mNotExecutionRows > 0)
        {
            mResult.mDiagnostics.emplace_back(
                fileDiagnostic(DiagnosticSeverity::Info,
                               DiagnosticCode::UnsupportedRowType,
                               mFile,
                               makeDiagnosticMessage(MessageKey::UnsupportedRowTypeNotExecution,
                                                     {numberParam("count", mNotExecutionRows)})));
        }

        if (mCurrencyConversionRows > 0)
        {
            mResult.mDiagnostics.emplace_back(fileDiagnostic(
                DiagnosticSeverity::Info,
                DiagnosticCode::UnsupportedRowType,
                mFile,
                makeDiagnosticMessage(MessageKey::UnsupportedRowTypeCurrencyConversion,
                                      {numberParam("count", mCurrencyConversionRows)})));
        }

        if (mNotDetailRows > 0)
        {
            mResult.mDiagnostics.emplace_back(
                fileDiagnostic(DiagnosticSeverity::Info,
                               DiagnosticCode::UnsupportedRowType,
                               mFile,
                               makeDiagnosticMessage(MessageKey::UnsupportedRowTypeNotDetail,
                                                     {numberParam("count", mNotDetailRows)})));
        }

        const auto& statement = mResult.mStatement;
        std::size_t trades = 0;
        std::size_t actions = 0;

        for (const auto& instrument : statement.mTradeInstruments)
        {
            trades += instrument.mTransactions.size();
            actions += instrument.mCorporateActions.size();
        }

        LOG_INFO(
            "Interactive Brokers XML source {}: {} statements, {} rows seen; trades {}, "
            "corporate actions {}, bond maturities {}, payment rows {}; left out {}, "
            "not executions {}, not detail rows {}, currency conversions {}, cash rows ignored {}, "
            "corporate action rows ignored {}",
            mSourceIndex,
            mStatements,
            mNextEventIndex,
            trades,
            actions,
            statement.mBondRedemptions.size(),
            statement.mIncomePaymentRows.size(),
            statement.mLeftOutRows.size(),
            mNotExecutionRows,
            mNotDetailRows,
            mCurrencyConversionRows,
            mIgnoredCashRows,
            mIgnoredActionRows);

        if (!statement.mLeftOutRows.empty())
        {
            LOG_INFO(
                "Interactive Brokers XML source {}: {} margin or leverage rows are left out of "
                "every form",
                mSourceIndex,
                statement.mLeftOutRows.size());
        }
    }

  private:
    /// The line of the row's start tag (1-based); 0 when the position is not known.
    [[nodiscard]] std::size_t lineOf(const pugi::xml_node& aRow) const {
        const auto offset = aRow.offset_debug();

        if (offset < 0)
        {
            return 0;
        }

        const auto next = std::upper_bound(mLineStarts.begin(),
                                           mLineStarts.end(),
                                           static_cast<std::size_t>(offset));

        return static_cast<std::size_t>(next - mLineStarts.begin());
    }

    [[nodiscard]] RowContext makeContext(const pugi::xml_node& aRow,
                                         const std::optional<ParsedRowKind> aKind) {
        return RowContext{
            .mResult = mResult,
            .mFile = mFile,
            .mLine = lineOf(aRow),
            .mSequence =
                StableInputSequence{
                    .mSourceIndex = mSourceIndex,
                    .mEventIndex = mNextEventIndex++,
                },
            .mTransactionId = textOf(aRow, "transactionID"),
            .mRowKind = aKind,
        };
    }

    /// Records a row that is left out of every form (a derivative, margin interest), with or
    /// without a transaction ID.
    void leftOut(const RowContext& aContext, const std::string_view aDateText) {
        // The row has no form, so a missing ID is no error: at worst the count of the later
        // warning is too high by the rows that two overlapping files repeat.
        // The date is only the identity of the row in overlapping files; a bad one is not an error
        // because nothing is read from the row.
        const auto date = dayOf(aDateText).value_or(Date{});

        LOG_DEBUG("Interactive Brokers XML source {} line {}: row left out of every form",
                  aContext.mSequence.mSourceIndex,
                  aContext.mLine);

        mResult.mStatement.mLeftOutRows.push_back(LeftOutRow{
            .mMetadata = aContext.metadata(date, std::nullopt),
            .mReason = LeftOutReason::MarginOrLeverage,
        });
    }

    void readTrades(const pugi::xml_node& aSection) {
        for (const pugi::xml_node row : aSection.children())
        {
            // A closed lot, an order or any other child of the trades section is not an execution.
            if (std::string_view{row.name()} != "Trade")
            {
                ++mNotExecutionRows;
                continue;
            }

            readTrade(row);
        }
    }

    void readTrade(const pugi::xml_node& aRow) {
        auto context = makeContext(aRow, ParsedRowKind::Trade);

        if (const auto* missing = firstMissingAttribute(aRow, kTradeBaseAttributes))
        {
            context.rejectMissingAttribute(missing);
            return;
        }

        // An empty value is a missing field: a notice would hide a lost trade.
        if (textOf(aRow, "levelOfDetail").empty())
        {
            context.rejectField("levelOfDetail", "");
            return;
        }

        if (textOf(aRow, "levelOfDetail") != "EXECUTION")
        {
            ++mNotExecutionRows;
            return;
        }

        const auto category = textOf(aRow, "assetCategory");
        const auto assetClass = assetClassOf(category, textOf(aRow, "subCategory"));

        if (category == "CASH")
        {
            ++mCurrencyConversionRows;
            LOG_DEBUG("Interactive Brokers XML source {} line {}: currency conversion not read",
                      mSourceIndex,
                      context.mLine);
            return;
        }

        if (assetClass == AssetClass::Derivative)
        {
            leftOut(context, textOf(aRow, "tradeDate"));
            return;
        }

        // IBKR writes crypto without an ISIN, and the app never makes one up.
        if (assetClass == AssetClass::Crypto && textOf(aRow, "isin").empty())
        {
            context.informNoForm(
                DiagnosticCode::UnsupportedRowType,
                makeDiagnosticMessage(MessageKey::UnsupportedRowTypeCryptoWithoutIsin));
            return;
        }

        if (const auto* attribute = cancellationAttribute(aRow))
        {
            context.reject(DiagnosticCode::UnsupportedRowType,
                           makeDiagnosticMessage(MessageKey::UnsupportedRowTypeCancelledTrade),
                           attribute);
            return;
        }

        const auto* missing = firstMissingAttribute(aRow, kTradeAttributes);

        if (missing == nullptr && aRow.attribute("isin").empty())
        {
            missing = "isin";
        }

        if (missing != nullptr)
        {
            context.rejectMissingAttribute(missing);
            return;
        }

        const auto fields = readTradeFields(aRow, context);

        if (!fields)
        {
            return;
        }

        storeTrade(aRow, context, *fields, assetClass);
    }

    void storeTrade(const pugi::xml_node& aRow,
                    const RowContext& aContext,
                    const TradeFields& aFields,
                    const AssetClass aAssetClass) {
        const std::string isin{textOf(aRow, "isin")};
        const std::string name{textOf(aRow, "description")};
        const std::string label{textOf(aRow, "assetCategory")};
        auto& instrument = getOrCreateInstrument(mResult.mStatement.mTradeInstruments, isin, name);

        aContext.preserveAssetClass(instrument, aAssetClass, label);

        // The amount is informational. A mismatch is reported, never repaired, and the row is kept.
        if (exceedsTradeAmountTolerance(aFields.mUnitPrice, aFields.mUnits, aFields.mAmount))
        {
            aContext.diagnose(DiagnosticSeverity::Warning,
                              DiagnosticCode::TradeAmountMismatch,
                              makeDiagnosticMessage(MessageKey::TradeAmountMismatch,
                                                    {textParam("field", "proceeds")}),
                              "proceeds");
        }

        auto metadata = aContext.metadata(aFields.mTaxDate, aFields.mTimestamp);
        auto unitEvidence =
            unitSourceEvidence(aFields.mQuantity, textOf(aRow, "quantity"), aContext.source());
        auto instrumentEvidence = aContext.instrumentEvidence(name, aAssetClass, label);

        instrument.mTransactions.emplace_back(TradeTransaction{
            .mMetadata = std::move(metadata),
            .mTradeSide = aFields.mSide,
            .mUnitPrice = aFields.mUnitPrice,
            .mUnits = aFields.mUnits,
            .mAmount = aFields.mAmount,
            .mFeePaid = aFields.mFee,
            .mExchangeRate = std::nullopt,
            .mCurrency = aFields.mCurrency,
            .mFeeCurrency = aFields.mFeeCurrency,
            .mTransactionTaxPaid = aFields.mTransactionTax,
            .mUnitEvidence = {std::move(unitEvidence)},
            .mInstrumentEvidence = std::move(instrumentEvidence),
        });
    }

    void readCorporateActions(const pugi::xml_node& aSection) {
        countUnexpectedChildren(aSection, "CorporateAction");

        // A split whose ISIN changes has rows of two ISINs with one action ID: it is not a split
        // of one holding, so it is left to the unsupported path.
        std::map<std::string, std::set<std::string>> isinsOfAction;

        for (const pugi::xml_node row : aSection.children("CorporateAction"))
        {
            const std::string actionId{textOf(row, "actionID")};

            if (!actionId.empty() && actionTypeOf(textOf(row, "type")) == ActionType::Split)
            {
                isinsOfAction[actionId].insert(std::string{textOf(row, "isin")});
            }
        }

        for (const pugi::xml_node row : aSection.children("CorporateAction"))
        {
            const auto isins = isinsOfAction.find(std::string{textOf(row, "actionID")});
            const bool changesIsin = isins != isinsOfAction.end() && isins->second.size() > 1;

            readCorporateAction(row, changesIsin);
        }
    }

    void readCorporateAction(const pugi::xml_node& aRow, const bool aChangesIsin) {
        auto context = makeContext(aRow, ParsedRowKind::CorporateAction);

        if (const auto* missing = firstMissingAttribute(aRow, kRowBaseAttributes))
        {
            context.rejectMissingAttribute(missing);
            return;
        }

        const auto category = textOf(aRow, "assetCategory");
        const auto assetClass = assetClassOf(category, "");
        const auto type = actionTypeOf(textOf(aRow, "type"));

        if (assetClass == AssetClass::Derivative)
        {
            leftOut(context, textOf(aRow, "dateTime"));
            return;
        }

        // The cash row of a dividend or a coupon is the payment; this row adds nothing.
        if (type == ActionType::CashPayment)
        {
            ++mIgnoredActionRows;
            LOG_DEBUG(
                "Interactive Brokers XML source {} line {}: corporate action row ignored, the "
                "cash row is the payment",
                mSourceIndex,
                context.mLine);
            return;
        }

        const bool isMaturity = type == ActionType::Maturity && assetClass == AssetClass::Bond;

        if (isMaturity)
        {
            context.mRowKind = ParsedRowKind::BondRedemption;
        }

        if (const auto* missing = firstMissingAttribute(aRow, kCorporateActionAttributes))
        {
            context.rejectMissingAttribute(missing);
            return;
        }

        if (isMaturity && aRow.attribute("proceeds").empty())
        {
            context.rejectMissingAttribute("proceeds");
            return;
        }

        if (context.rejectEmptyTransactionId())
        {
            return;
        }

        const auto isin = textOf(aRow, "isin");

        if (isin.empty())
        {
            context.rejectField("isin", isin);
            return;
        }

        const auto dateTime = parseCompactDateTime(textOf(aRow, "dateTime"), TimeOfDay::Required);

        if (!dateTime)
        {
            context.rejectField("dateTime", textOf(aRow, "dateTime"));
            return;
        }

        const auto currency = parseCurrencyCode(textOf(aRow, "currency"));

        if (currency == Currency::Unknown)
        {
            context.rejectField("currency", textOf(aRow, "currency"));
            return;
        }

        const auto quantity = readFixedPoint(textOf(aRow, "quantity"), UNITS_SCALE);
        const bool needsUnits = type == ActionType::Split || isMaturity;

        if (!quantity || (needsUnits && quantity->mValue == 0) ||
            quantity->mValue == std::numeric_limits<Units>::min())
        {
            context.rejectField("quantity", textOf(aRow, "quantity"));
            return;
        }

        if (isMaturity)
        {
            storeMaturity(aRow, context, *dateTime, currency, *quantity);
            return;
        }

        storeAction(aRow, context, *dateTime, *quantity, type, aChangesIsin);
    }

    void storeAction(const pugi::xml_node& aRow,
                     const RowContext& aContext,
                     const CompactDateTime& aDateTime,
                     const ParsedFixedPoint& aQuantity,
                     const ActionType aType,
                     const bool aChangesIsin) {
        const std::string isin{textOf(aRow, "isin")};
        const std::string name{textOf(aRow, "description")};
        auto& instrument = getOrCreateInstrument(mResult.mStatement.mTradeInstruments, isin, name);

        // The text of the action (for example "SPLIT 4 FOR 1") and the quantity are evidence only,
        // never a verified ratio. A merger and every action that is not supported is the type
        // the corporate-action stage blocks. The row gives no class evidence (see above).
        const bool isSplit = aType == ActionType::Split && !aChangesIsin;
        auto metadata = aContext.metadata(aDateTime.mDate, aDateTime.mTimestamp);
        auto unitEvidence =
            unitSourceEvidence(aQuantity, textOf(aRow, "quantity"), aContext.source());

        instrument.mCorporateActions.emplace_back(CorporateAction{
            .mMetadata = std::move(metadata),
            .mType = isSplit ? CorporateActionType::UnresolvedSplit : CorporateActionType::Merger,
            .mUnitsDelta = aQuantity.mValue,
            .mRatio = std::nullopt,
            .mUnitEvidence = {std::move(unitEvidence)},
            .mInstrumentEvidence = {},
        });
    }

    void storeMaturity(const pugi::xml_node& aRow,
                       const RowContext& aContext,
                       const CompactDateTime& aDateTime,
                       const Currency aCurrency,
                       const ParsedFixedPoint& aQuantity) {
        const auto proceeds = readMoney(textOf(aRow, "proceeds"));

        if (!proceeds)
        {
            aContext.rejectField("proceeds", textOf(aRow, "proceeds"));
            return;
        }

        // Units leave the holding when the bond matures; a positive quantity gives them back.
        const bool isCancelled = aQuantity.mValue > 0;

        mResult.mStatement.mBondRedemptions.push_back(BondRedemptionEvent{
            .mMetadata = aContext.metadata(aDateTime.mDate, aDateTime.mTimestamp),
            .mKind =
                isCancelled ? BondRedemptionKind::MaturityCancelled : BondRedemptionKind::Maturity,
            .mName = std::string{textOf(aRow, "description")},
            .mIsin = std::string{textOf(aRow, "isin")},
            .mUnits = std::abs(aQuantity.mValue),
            .mAmount = std::abs(*proceeds),
            .mRedemptionPrice = std::nullopt,
            .mFeePaid = 0,
            .mTaxPaid = std::nullopt,
            .mExchangeRate = std::nullopt,
            .mCurrency = aCurrency,
            .mFeeCurrency = aCurrency,
        });
    }

    void readCashTransactions(const pugi::xml_node& aSection) {
        countUnexpectedChildren(aSection, "CashTransaction");

        for (const pugi::xml_node row : aSection.children("CashTransaction"))
        {
            readCashTransaction(row);
        }
    }

    /// Counts the children of a section that are not rows of the expected element. They are not
    /// read, and the file leaves one notice with their number.
    void countUnexpectedChildren(const pugi::xml_node& aSection, const char* aRowName) {
        const auto children = aSection.children();

        mNotDetailRows += static_cast<std::size_t>(
            std::count_if(children.begin(),
                          children.end(),
                          [aRowName](const pugi::xml_node& aChild) {
                              return std::string_view{aChild.name()} != aRowName;
                          }));
    }

    void readCashTransaction(const pugi::xml_node& aRow) {
        auto context = makeContext(aRow, std::nullopt);

        // Only the single bookings are read. A total row would count a booking twice, and it may
        // have no ID or ISIN, so the level is checked before anything else.
        if (aRow.attribute("levelOfDetail").empty())
        {
            context.rejectMissingAttribute("levelOfDetail");
            return;
        }

        if (textOf(aRow, "levelOfDetail").empty())
        {
            context.rejectField("levelOfDetail", "");
            return;
        }

        if (textOf(aRow, "levelOfDetail") != "DETAIL")
        {
            ++mNotDetailRows;
            return;
        }

        if (const auto* missing = firstMissingAttribute(aRow, kRowBaseAttributes))
        {
            context.rejectMissingAttribute(missing);
            return;
        }

        const auto type = cashTypeOf(textOf(aRow, "type"));
        const auto assetClass = assetClassOf(textOf(aRow, "assetCategory"), "");

        // Any row of a derivative is left out of every form.
        if (assetClass == AssetClass::Derivative)
        {
            leftOut(context, textOf(aRow, "dateTime"));
            return;
        }

        switch (type)
        {
        case CashType::Ignored:
            ++mIgnoredCashRows;
            LOG_DEBUG(
                "Interactive Brokers XML source {} line {}: cash row ignored, it is not income",
                mSourceIndex,
                context.mLine);
            return;
        case CashType::Unknown:
            context.reject(
                DiagnosticCode::UnknownRowType,
                makeDiagnosticMessage(MessageKey::UnknownRowType,
                                      {textParam("type", std::string{textOf(aRow, "type")}),
                                       textParam("category", "CashTransaction")}),
                "type");
            return;
        case CashType::Dividend:
        case CashType::WithholdingTax:
        case CashType::PaymentInLieu:
            context.mRowKind = ParsedRowKind::Dividend;
            readPaymentRow(aRow, context, type);
            return;
        case CashType::BrokerInterestReceived:
            context.mRowKind = ParsedRowKind::BrokerInterest;
            readInterestRow(aRow, context, false);
            return;
        case CashType::BondInterestReceived:
            context.mRowKind = ParsedRowKind::BondInterest;
            readInterestRow(aRow, context, true);
            return;
        case CashType::BrokerInterestPaid:
            // Margin interest is left out of every form too.
            leftOut(context, textOf(aRow, "dateTime"));
            return;
        }
    }

    /// The date, currency and amount that every income row needs.
    struct CashFields {
        CompactDateTime mDateTime;
        Currency mCurrency{};
        Money mAmount{};
    };

    [[nodiscard]] static std::optional<CashFields> readCashFields(const pugi::xml_node& aRow,
                                                                  const RowContext& aContext) {
        const auto dateTime = parseCompactDateTime(textOf(aRow, "dateTime"), TimeOfDay::Optional);

        if (!dateTime)
        {
            return aContext.rejectField("dateTime", textOf(aRow, "dateTime"));
        }

        const auto currency = parseCurrencyCode(textOf(aRow, "currency"));

        if (currency == Currency::Unknown)
        {
            return aContext.rejectField("currency", textOf(aRow, "currency"));
        }

        const auto amount = readMoney(textOf(aRow, "amount"));

        if (!amount)
        {
            return aContext.rejectField("amount", textOf(aRow, "amount"));
        }

        return CashFields{.mDateTime = *dateTime, .mCurrency = currency, .mAmount = *amount};
    }

    /// One cash row of a dividend payment: a dividend, a tax, a reversal or a refund.
    void readPaymentRow(const pugi::xml_node& aRow, RowContext& aContext, const CashType aType) {
        // A tax row that names no instrument cannot be added to a payment, and the tax may be on a
        // dividend or on interest, so its error names both kinds of form.
        if (aType == CashType::WithholdingTax && textOf(aRow, "isin").empty())
        {
            aContext.mRowKind = ParsedRowKind::IncomeTax;
        }

        for (const char* attribute : {"isin", "actionID", "reportDate"})
        {
            if (aRow.attribute(attribute).empty())
            {
                aContext.rejectMissingAttribute(attribute);
                return;
            }
        }

        if (const auto* missing = firstMissingAttribute(aRow, kCashAttributes))
        {
            aContext.rejectMissingAttribute(missing);
            return;
        }

        if (aContext.rejectEmptyTransactionId())
        {
            return;
        }

        // A payment is netted by its action ID and its ISIN, so a row without either is rejected.
        if (textOf(aRow, "isin").empty())
        {
            aContext.rejectField("isin", "");
            return;
        }

        if (textOf(aRow, "actionID").empty())
        {
            aContext.rejectField("actionID", "");
            return;
        }

        const auto fields = readCashFields(aRow, aContext);
        const auto bookedDate = parseCompactDate(textOf(aRow, "reportDate"));

        if (!fields)
        {
            return;
        }

        if (!bookedDate)
        {
            aContext.rejectField("reportDate", textOf(aRow, "reportDate"));
            return;
        }

        const bool isNegative = fields->mAmount < 0;
        IncomePaymentKind kind =
            isNegative ? IncomePaymentKind::DividendReversal : IncomePaymentKind::Dividend;

        if (aType == CashType::WithholdingTax)
        {
            kind = isNegative || fields->mAmount == 0 ? IncomePaymentKind::Tax
                                                      : IncomePaymentKind::TaxRefund;
        }

        const auto assetClass = assetClassOf(textOf(aRow, "assetCategory"), "");

        mResult.mStatement.mIncomePaymentRows.push_back(IncomePaymentRow{
            .mMetadata = aContext.metadata(fields->mDateTime.mDate, fields->mDateTime.mTimestamp),
            .mKind = kind,
            .mName = "",
            .mIsin = std::string{textOf(aRow, "isin")},
            .mAmount = std::abs(fields->mAmount),
            .mCurrency = fields->mCurrency,
            .mBookedDate = *bookedDate,
            .mPaymentGroupId = std::string{textOf(aRow, "actionID")},
            .mSubstitutePayment = aType == CashType::PaymentInLieu,
            .mInstrumentEvidence = {InstrumentSourceEvidence{
                .mSource = aContext.source(),
                .mName = "",
                .mAssetClass = assetClass,
                .mAssetClassLabel = std::string{textOf(aRow, "assetCategory")},
            }},
        });
    }

    /// Interest on the broker's cash, or a bond coupon.
    void readInterestRow(const pugi::xml_node& aRow, RowContext& aContext, const bool aIsBond) {
        if (aIsBond && aRow.attribute("isin").empty())
        {
            aContext.rejectMissingAttribute("isin");
            return;
        }

        if (const auto* missing = firstMissingAttribute(aRow, kCashAttributes))
        {
            aContext.rejectMissingAttribute(missing);
            return;
        }

        if (aContext.rejectEmptyTransactionId())
        {
            return;
        }

        if (aIsBond && textOf(aRow, "isin").empty())
        {
            aContext.rejectField("isin", "");
            return;
        }

        const auto fields = readCashFields(aRow, aContext);

        if (!fields)
        {
            return;
        }

        // Cash interest can be negative (debit interest); a coupon cannot.
        if (aIsBond && fields->mAmount < 0)
        {
            aContext.rejectField("amount", textOf(aRow, "amount"));
            return;
        }

        const InterestType interestType =
            aIsBond ? InterestType::BondInterest : InterestType::BrokerInterest;
        std::optional<Isin> isin;

        if (aIsBond)
        {
            isin = std::string{textOf(aRow, "isin")};
        }

        auto& instrument = interestInstrument(interestType, isin);
        std::vector<InstrumentSourceEvidence> evidence;

        // The coupon says which bond it is; interest on cash has no instrument and no name.
        if (aIsBond)
        {
            evidence.push_back(InstrumentSourceEvidence{
                .mSource = aContext.source(),
                .mName = "",
                .mAssetClass = AssetClass::Bond,
                .mAssetClassLabel = std::string{textOf(aRow, "assetCategory")},
            });
        }

        instrument.mTransactions.emplace_back(InterestTransaction{
            .mMetadata = aContext.metadata(fields->mDateTime.mDate, fields->mDateTime.mTimestamp),
            .mGrossAmount = fields->mAmount,
            .mTaxPaid = std::nullopt,
            .mExchangeRate = std::nullopt,
            .mCurrency = fields->mCurrency,
            .mTaxCurrency = fields->mCurrency,
            .mInstrumentEvidence = std::move(evidence),
            .mCashHolder = aIsBond ? std::nullopt : std::optional{CashHolder::Broker},
            .mPayerCode = aIsBond || mEntityCode.empty() ? std::nullopt
                                                         : std::optional<std::string>{mEntityCode},
        });
    }

    /// The interest instrument of the kind and ISIN, made on first use.
    InterestInstrument& interestInstrument(const InterestType aType,
                                           const std::optional<Isin>& aIsin) {
        auto& instruments = mResult.mStatement.mInterestInstruments;
        auto instrument =
            std::find_if(instruments.begin(),
                         instruments.end(),
                         [&](const InterestInstrument& aCandidate) {
                             return aCandidate.mInterestType == aType && aCandidate.mIsin == aIsin;
                         });

        if (instrument == instruments.end())
        {
            instruments.emplace_back(
                InterestInstrument{.mName = "", .mIsin = aIsin, .mInterestType = aType});
            instrument = std::prev(instruments.end());
        }

        return *instrument;
    }

    ParseResult& mResult;
    const SourceFilename& mFile;
    std::size_t mSourceIndex;
    std::vector<std::size_t> mLineStarts; ///< Offset of the first character of each line.
    std::string mEntityCode;              ///< `ibEntity` of the statement being read.
    std::size_t mNextEventIndex{};
    std::size_t mNotExecutionRows{};
    std::size_t mNotDetailRows{};
    std::size_t mCurrencyConversionRows{};
    std::size_t mStatements{};
    std::size_t mIgnoredCashRows{};
    std::size_t mIgnoredActionRows{};
};

} // namespace

ParseResult IbkrParser::parse(const std::string_view aXmlText,
                              const std::string_view aDisplayFilename,
                              const std::size_t aSourceIndex) {
    ParseResult result{
        .mBroker = Broker::InteractiveBrokers,
    };
    const auto validFilename = SourceFilename::tryFromPath(aDisplayFilename);

    if (!validFilename)
    {
        LOG_ERROR("Interactive Brokers XML source {} has no valid file name", aSourceIndex);
        result.mDiagnostics.emplace_back(
            fileDiagnostic(DiagnosticSeverity::Error,
                           DiagnosticCode::ParseError,
                           SourceFilename{},
                           makeDiagnosticMessage(MessageKey::ParseErrorInvalidFilename)));

        return result;
    }

    const SourceFilename& sourceFile = *validFilename;

    if (aXmlText.size() > maxInputBytes())
    {
        result.mDiagnostics.emplace_back(inputFileFaultDiagnostic(InputFileFault::TooLarge,
                                                                  sourceFile.value(),
                                                                  brokerName(broker()),
                                                                  aSourceIndex,
                                                                  maxInputBytes()));

        return result;
    }

    LOG_INFO("Interactive Brokers XML source {}: reading {} bytes", aSourceIndex, aXmlText.size());

    try
    {
        // A DTD (the <!DOCTYPE ...> line) can define entities that pull in local files or URLs.
        // We reject any file that has one. pugixml keeps it as a node only so we can detect it.
        pugi::xml_document document;
        const auto loaded = document.load_buffer(aXmlText.data(),
                                                 aXmlText.size(),
                                                 pugi::parse_default | pugi::parse_doctype,
                                                 pugi::encoding_utf8);

        if (loaded.status == pugi::status_out_of_memory)
        {
            throw std::bad_alloc{};
        }

        const bool hasDoctype =
            loaded.status == pugi::status_bad_doctype ||
            std::any_of(document.begin(), document.end(), [](const pugi::xml_node& aNode) {
                return aNode.type() == pugi::node_doctype;
            });

        if (hasDoctype)
        {
            LOG_ERROR("Interactive Brokers XML source {} has a document type", aSourceIndex);
            result.mDiagnostics.emplace_back(
                fileDiagnostic(DiagnosticSeverity::Error,
                               DiagnosticCode::InvalidHeader,
                               sourceFile,
                               makeDiagnosticMessage(MessageKey::InvalidHeaderDoctypeNotAllowed)));

            return result;
        }

        const auto root = document.child("FlexQueryResponse");

        if (!loaded || root.empty())
        {
            LOG_ERROR("Interactive Brokers XML source {} is not a Flex Query XML", aSourceIndex);
            result.mDiagnostics.emplace_back(fileDiagnostic(
                DiagnosticSeverity::Error,
                DiagnosticCode::InvalidHeader,
                sourceFile,
                makeDiagnosticMessage(MessageKey::InvalidHeaderFlexQueryXmlRequired)));

            return result;
        }

        FlexReader reader{result, sourceFile, aSourceIndex, aXmlText};

        for (const pugi::xml_node statement :
             root.child("FlexStatements").children("FlexStatement"))
        {
            reader.readStatement(statement);
        }

        reader.finish();
    } catch (const std::bad_alloc&)
    {
        // Discard partial results after a file-level failure.
        result.mStatement = {};
        result.mDiagnostics.emplace_back(inputFileFaultDiagnostic(InputFileFault::OutOfMemory,
                                                                  sourceFile.value(),
                                                                  brokerName(broker()),
                                                                  aSourceIndex,
                                                                  maxInputBytes()));
    }

    return result;
}

} // namespace taxbroker::ibkr
