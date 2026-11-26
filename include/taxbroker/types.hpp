#pragma once

#include "taxbroker/diagnostics.hpp"
#include "taxbroker/event_metadata.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

/*
    Fixed-point monetary representation with 4 decimal precision.
    Example:
        123456 -> 12.3456

    Using scaled integers avoids floating-point precision issues
    during FIFO matching and tax calculations.
*/
using Money = std::int64_t;
constexpr Money MONEY_SCALE = 10000;

/*
    Fixed-point unit price with 8 decimal precision, the limit FURS accepts for the
    Doh-KDVP unit values F4 and F9.
    Example:
        1234567891 -> 12.34567891
*/
using UnitPrice = std::int64_t;
constexpr UnitPrice UNIT_PRICE_SCALE = 100000000;

using ExchangeRate = std::int64_t;
constexpr ExchangeRate EXCHANGE_RATE_SCALE = 100000000;

/// What a rate means: ForeignPerEur is `1 EUR = rate` foreign units (the official direction),
/// EurPerForeign is `1 foreign unit = rate` EUR.
enum class ExchangeRateDirection {
    ForeignPerEur,
    EurPerForeign,
};

/// How a broker rate's direction is known. Only a verified direction may convert an amount.
enum class RateDirectionVerification {
    Unverified,
    /// Export samples fit only this direction (Trade Republic income).
    VerifiedBySourceSamples,
    /// No sample exists; the maintainer decided the direction (Trade Republic trades, 2026-10-20).
    VerifiedByMaintainerDecision,
};

/// A broker rate with the direction it is read in and how that direction is known.
struct BrokerExchangeRate {
    ExchangeRate mValue{};
    ExchangeRateDirection mDirection{ExchangeRateDirection::EurPerForeign};
    RateDirectionVerification mVerification{RateDirectionVerification::Unverified};

    bool operator==(const BrokerExchangeRate&) const = default;
};

/*
    Fixed-point asset/unit representation with 8 decimal precision.
    Supports fractional shares and precise partial trade matching.
    Example:
        123456789 -> 1.23456789
*/
using Units = std::int64_t;
constexpr Units UNITS_SCALE = 100000000;

/*
    Fixed-point ratio representation with 8 decimal precision.
    Example:
        125000000 -> 1.25000000
*/
using CorpRatio = std::int64_t;
constexpr CorpRatio CORP_RATIO_SCALE = 100000000;

/// An exact split ratio: new shares per old shares, both positive and reduced, never equal.
/// Example: 2 / 1 means every old share becomes two shares.
struct SplitRatio {
    std::int64_t mNewShares{};
    std::int64_t mOldShares{};

    bool operator==(const SplitRatio&) const = default;
};

using Isin = std::string;

enum class TradeSide {
    Buy,
    Sell,
};

/// EUR and the currencies with an official ECB reference rate since 2020 (docs/calculations.md,
/// "Supported currencies"). Unknown stays last: the code table in currency.cpp counts on it.
enum class Currency {
    EUR,
    USD,
    GBP,
    CHF,
    JPY,
    AUD,
    BGN,
    BRL,
    CAD,
    CNY,
    CZK,
    DKK,
    HKD,
    HRK,
    HUF,
    IDR,
    ILS,
    INR,
    ISK,
    KRW,
    MXN,
    MYR,
    NOK,
    NZD,
    PHP,
    PLN,
    RON,
    RUB,
    SEK,
    SGD,
    THB,
    TRY,
    ZAR,
    Unknown
};

enum class AssetClass {
    Stock,
    Fund,
    Bond,
    Derivative,
    Crypto,
    PrivateFund,
    Unknown
};

// Potentially useful for debugging and logging
enum class EventType {
    Trade,
    Dividend,
    Interest
};

enum class CorporateActionType {
    Split,
    ReverseSplit,
    /// Any action the app does not support (a spin-off, rights, a tender, a stock dividend, ...).
    Merger,
    UnresolvedSplit,
    /// Units of one ISIN leave and units of another ISIN come in, one action. The user is asked
    /// whether it is the same issuer, the same kind of share and no cash (see mNewIsin).
    IsinChange,
    /// Shares of one company become shares of another, with or without cash. The stock merger
    /// stage asks how it is taxed; a merger it cannot read stays unsupported.
    StockMerger,
    /// A StockMerger row of a merger the stock merger stage asked about or blocked: the stage has
    /// its own question or error, so no other stage adds one.
    StockMergerPending
};

/// True for a row that is not a split or a reverse split. The corporate-action stage never asks
/// the ratio of such a row; it is unsupported, or an ISIN change that has its own question.
[[nodiscard]] constexpr bool isNonSplitActionType(CorporateActionType aType) noexcept {
    return aType == CorporateActionType::Merger || aType == CorporateActionType::StockMerger ||
           aType == CorporateActionType::StockMergerPending ||
           aType == CorporateActionType::IsinChange;
}

enum class InterestType {
    BondInterest,
    BrokerInterest,
    OtherInterest,
    UnknownInterest
};

enum class BenefitType {
    Saveback,
    StockPerk,
    Bonus,
    Referral
};

enum class PrivateMarketEventType {
    Buy,
    Sell
};

/// Preserves pre-rounding quantities for duplicate checks and later FIFO management.
struct UnitSourceEvidence {
    SourceReference mSource;
    std::string mSourceText;
    std::string mDiscardedDigits;
    std::string mCanonicalValue; ///< Exact signed source decimal without redundant zeros.
    bool mRoundedAwayFromZero{}; ///< Import rounding increased the magnitude.

    bool operator==(const UnitSourceEvidence&) const = default;
};

/// Original row metadata stays attached to the event, independently of instrument presentation.
struct InstrumentSourceEvidence {
    SourceReference mSource;
    std::string mName;
    AssetClass mAssetClass{AssetClass::Unknown};
    std::string mAssetClassLabel;

    bool operator==(const InstrumentSourceEvidence&) const = default;
};

struct CorporateAction {
    EventMetadata mMetadata;
    CorporateActionType mType{CorporateActionType::UnresolvedSplit};
    /// Rounded source quantity for UnresolvedSplit, not a verified inventory change.
    /// Its sign does not establish direction; mUnitEvidence retains the exact source decimal.
    Units mUnitsDelta{};
    /// A verified ratio from the source, kept exact. Empty when the source gives none.
    std::optional<SplitRatio> mRatio;
    /// The broker's own ID of the economic action, shared by all its rows (one per account).
    /// Empty when the export has none. IDs of different brokers are never compared.
    std::optional<std::string> mBrokerActionId;
    /// For IsinChange on the row of the old ISIN: the ISIN the units become. Empty on every other
    /// row, also on the row of the new ISIN.
    std::optional<std::string> mNewIsin;
    /// For StockMerger on a row that loses units: the cash the broker paid with it, positive, in
    /// mAmountCurrency. Empty when the export gives none.
    std::optional<Money> mCashProceeds;
    /// For StockMerger on a row that gets units: the market value the broker gives for them,
    /// positive, in mAmountCurrency. Evidence only: the user confirms the value. Empty when none.
    std::optional<Money> mMarketValue;
    /// Currency of the row's amounts (mCashProceeds, mMarketValue); Unknown when the source gives
    /// none.
    Currency mAmountCurrency{Currency::Unknown};
    std::vector<UnitSourceEvidence> mUnitEvidence;
    std::vector<InstrumentSourceEvidence> mInstrumentEvidence;
};

/// How a purchase came about, for Doh-KDVP F2.
enum class PurchaseKind {
    NotAPurchase,         ///< A sale: there is no way of purchase.
    Bought,               ///< Bought: F2 `B`.
    StatusChangeExchange, ///< Shares received in an exchange at a status change: F2 `E`.
};

/// The ISIN and name an instrument had when a trade was made.
struct ListedInstrument {
    Isin mIsin;
    std::string mName;

    bool operator==(const ListedInstrument&) const = default;
};

struct TradeTransaction {
    EventMetadata mMetadata;
    TradeSide mTradeSide{};
    UnitPrice mUnitPrice{};
    Units mUnits{};
    /// Positive transaction value from the broker export. Some executions omit it.
    std::optional<Money> mAmount;
    /// Positive fee paid; zero when the export has no fee.
    Money mFeePaid{};
    /// Optional broker rate with its direction, separate from the official rate selected during
    /// processing.
    std::optional<BrokerExchangeRate> mExchangeRate;
    /// Currency of the unit price and the amount.
    Currency mCurrency{Currency::Unknown};
    /// Currency of mFeePaid; the export's `currency` field.
    Currency mFeeCurrency{Currency::Unknown};
    /// Positive second fee the export books with the trade (stamp duty, financial transaction tax),
    /// in mCurrency; zero when there is none.
    Money mTransactionTaxPaid{};
    std::vector<UnitSourceEvidence> mUnitEvidence;
    std::vector<InstrumentSourceEvidence> mInstrumentEvidence;
    /// True for the units that left the holding in a takeover for cash only: a sale at the cash
    /// paid, at the date of the broker's action row, not a trade the user made.
    bool mFromCashMerger{};
    /// True for the units that left the holding in a stock merger the user answered as taxable:
    /// a sale at the value of the new shares plus the cash, not a trade the user made.
    bool mFromStockMerger{};
    /// How a purchase came about (Doh-KDVP F2); NotAPurchase for a sale. Always set by the creator
    /// of a trade: `{}` is only the neutral value, which the F2 of a purchase maps to `B`.
    PurchaseKind mPurchaseKind{};
    /// Set only by the ISIN change stage on a trade it moved to the new ISIN: the ISIN and name
    /// the trade had, under which a sale is listed. Empty on every other trade.
    std::optional<ListedInstrument> mListedAs;
};

struct TradeInstrument {
    std::string mName;
    Isin mIsin;
    AssetClass mAssetClass{AssetClass::Unknown};
    std::vector<TradeTransaction> mTransactions;
    std::vector<CorporateAction> mCorporateActions;
};

struct DividendTransaction {
    EventMetadata mMetadata;
    Money mGrossAmount{};
    /// Positive withholding in mTaxCurrency; absent when the source tax is blank, which means no
    /// tax was paid.
    std::optional<Money> mTaxPaid;
    /// Optional broker rate with its direction, separate from the official rate selected during
    /// processing.
    std::optional<BrokerExchangeRate> mExchangeRate;
    Currency mCurrency{Currency::EUR};
    Currency mTaxCurrency{Currency::EUR};
    std::vector<InstrumentSourceEvidence> mInstrumentEvidence;
    /// The ISO 3166-1 country of the payer as the export names it; absent when it does not.
    std::optional<std::string> mPayerCountry;
};

struct DividendInstrument {
    std::string mName;
    Isin mIsin;
    std::vector<DividendTransaction> mTransactions;
};

/// Who holds the cash an interest payment is paid on.
enum class CashHolder {
    Broker, ///< A broker only: Doh-Obr.
    Bank,   ///< A bank - the cash is a bank deposit: Doh-DHO.
};

struct InterestTransaction {
    EventMetadata mMetadata;
    Money mGrossAmount{};
    /// Positive withholding in mTaxCurrency; absent when the source tax is blank, which means no
    /// tax was paid.
    std::optional<Money> mTaxPaid;
    /// Optional broker rate with its direction, separate from the official rate selected during
    /// processing.
    std::optional<BrokerExchangeRate> mExchangeRate;
    Currency mCurrency{Currency::EUR};
    Currency mTaxCurrency{Currency::EUR};
    std::vector<InstrumentSourceEvidence> mInstrumentEvidence;
    /// Who held the cash on the receipt date; absent for a bond coupon and for a row of unknown
    /// type. Always set by the parser for the interest on cash.
    std::optional<CashHolder> mCashHolder;
    /// The `code` of the `broker_payers.csv` row of the entity that paid the interest on cash, when
    /// the export names it; absent otherwise (the payer is then found another way or stays empty).
    std::optional<std::string> mPayerCode;
    /// The ISO 3166-1 country of the payer as the export names it; absent when it does not (the
    /// payer of cash interest comes from `mPayerCode`).
    std::optional<std::string> mPayerCountry;
};

struct InterestInstrument {
    std::string mName;
    std::optional<Isin> mIsin;
    InterestType mInterestType{InterestType::UnknownInterest};
    std::vector<InterestTransaction> mTransactions;
};

/// A broker bonus credit. Information only: never units, a lot, a classification request or a
/// form, and it never changes a purchase value.
struct BenefitEvent {
    EventMetadata mMetadata;
    BenefitType mType{};
    std::string mName;
    std::optional<Isin> mIsin;
    AssetClass mAssetClass{AssetClass::Unknown};
    Money mAmount{};
    Currency mCurrency{Currency::EUR};
};

/// What one step of a bond maturity does. Every value is positive; the kind carries the sign.
enum class BondRedemptionKind {
    Maturity,          ///< Units leave the holding.
    MaturityCancelled, ///< Units come back (the maturity is cancelled).
    Payment,           ///< Money is paid out.
    PaymentReversal,   ///< Money is taken back (a cancelled payout).
};

/// One step of a bond maturity. Information only: it is not a sale, never reaches Doh-KDVP and
/// nothing downstream changes a lot or a basis because of it. A maturity can be cancelled and
/// booked again, so the steps are not added up here; the bond list and the coupon (Doh-Obr) work
/// do that later. Each parser documents how its rows map to the kinds (`docs/architecture.md`).
struct BondRedemptionEvent {
    EventMetadata mMetadata;
    BondRedemptionKind mKind{};
    std::string mName;
    Isin mIsin;
    std::optional<Units> mUnits;  ///< Positive units moved; absent when the source gives none.
    std::optional<Money> mAmount; ///< Positive money moved, in mCurrency; absent without one.
    std::optional<UnitPrice> mRedemptionPrice; ///< Price per unit, when the source gives one.
    Money mFeePaid{};              ///< Positive fee paid; zero when the export has no fee.
    std::optional<Money> mTaxPaid; ///< Positive tax withheld; absent means unknown.
    std::optional<BrokerExchangeRate> mExchangeRate; ///< Broker rate of a foreign amount.
    Currency mCurrency{Currency::Unknown};           ///< Currency of mAmount; Unknown without one.
    /// Currency of mFeePaid (the export's `currency` field); Unknown without an amount row.
    Currency mFeeCurrency{Currency::Unknown};
};

struct PrivateMarketEvent {
    EventMetadata mMetadata;
    PrivateMarketEventType mType{};
    std::string mName;
    std::optional<Isin> mIsin;
    AssetClass mAssetClass{AssetClass::Unknown};
    Money mAmount{};
    Money mFeePaid{};
    Currency mCurrency{Currency::EUR};
    std::string mDescription;
};

/// What one cash row of an income payment is. Every amount is positive; the kind carries the sign.
enum class IncomePaymentKind {
    Dividend,         ///< A dividend paid, a payment in lieu of one included.
    DividendReversal, ///< A dividend taken back.
    Tax,              ///< Foreign tax withheld.
    TaxRefund,        ///< Foreign tax given back.
};

/// One cash row of an income payment, before the rows of the payment are added up. A broker that
/// books a dividend, its tax and its corrections as separate rows (in separate files, too) fills
/// this list; a later step turns the rows of each payment into one dividend.
struct IncomePaymentRow {
    EventMetadata mMetadata;               ///< The tax date is the day the payment was made.
    IncomePaymentKind mKind{};             ///< Always set by the creator.
    std::string mName;                     ///< Empty when the row does not name the instrument.
    Isin mIsin;                            ///< The instrument the payment is for.
    Money mAmount{};                       ///< Positive; the kind carries the sign.
    Currency mCurrency{Currency::Unknown}; ///< Currency of mAmount; always set by the creator.
    std::optional<Date> mBookedDate;       ///< The day the broker booked the row.
    std::string mPaymentGroupId;           ///< The same for every row of one payment.
    bool mSubstitutePayment{};             ///< A payment in lieu of a dividend.
    std::vector<InstrumentSourceEvidence> mInstrumentEvidence; ///< Class evidence of the row.
    std::optional<std::string> mPayerCountry; ///< ISO 3166-1 country the export names; or none.
};

/// Why a row of the export is not read into any event.
enum class LeftOutReason {
    MarginOrLeverage, ///< A derivative, a leveraged product or margin interest.
};

/// A row that is left out of every form on purpose. The count per broker is reported once.
struct LeftOutRow {
    EventMetadata mMetadata;
    LeftOutReason mReason{}; ///< Always set by the creator.
};

struct BrokerStatement {
    std::vector<TradeInstrument> mTradeInstruments;
    std::vector<DividendInstrument> mDividendInstruments;
    std::vector<InterestInstrument> mInterestInstruments;
    std::vector<BenefitEvent> mBenefitEvents;
    std::vector<BondRedemptionEvent> mBondRedemptions; ///< Information only
    std::vector<PrivateMarketEvent> mPrivateMarketEvents;
    std::vector<IncomePaymentRow> mIncomePaymentRows; ///< Added up after the merge.
    std::vector<LeftOutRow> mLeftOutRows;             ///< Not an event; counted in one warning.
};

// Broker data with structured diagnostics used throughout the processing
// pipeline and by the frontend API.
struct ParseResult {
    Broker mBroker{Broker::Unknown};
    BrokerStatement mStatement;
    std::vector<ParseDiagnostic> mDiagnostics;
};

} // namespace taxbroker
