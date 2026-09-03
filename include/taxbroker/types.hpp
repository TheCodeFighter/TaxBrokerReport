#pragma once

#include "taxbroker/diagnostics.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <ratio>
#include <string>
#include <vector>

namespace taxbroker {

enum class Broker {
    Unknown,
    TradeRepublic,
    InteractiveBrokers,
};

/*
    Fixed-point monetary representation with 4 decimal precision.
    Example:
        123456 -> 12.3456

    Using scaled integers avoids floating-point precision issues
    during FIFO matching and tax calculations.
*/
using Money = std::int64_t;
constexpr Money MONEY_SCALE = 10000;

using ExchangeRate = std::int64_t;
constexpr ExchangeRate EXCHANGE_RATE_SCALE = 100000000;

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

/*
    Day-precision date representation used across the tax pipeline.
    Time-of-day is intentionally ignored since tax reporting is date-based.
*/
using DayDuration = std::chrono::duration<std::int64_t, std::ratio<86400>>;
using Date = std::chrono::time_point<std::chrono::system_clock, DayDuration>;

using Isin = std::string;

enum class TradeSide {
    Buy,
    Sell,
};

enum class Currency {
    EUR,
    USD,
    GBP,
    CHF,
    JPY,
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
    Merger
};

enum class InterestType {
    BondInterest,
    BrokerInterest,
    OtherInterest,
    UnknownInterest
};

enum class BenefitType {
    Saveback,
    Stockperk
};

enum class PrivateMarketEventType {
    Buy,
    Sell,
    Bonus
};

struct CorporateAction {
    Date mDate{};
    CorporateActionType mType{};
    Units mUnitsDelta{};
    std::optional<CorpRatio> mRatio;
    std::string mTransactionId;
};

struct TradeTransaction {
    Date mDate{};
    TradeSide mTradeSide{};
    Money mUnitPrice{};
    Units mUnits{};
    // Positive transaction value from the broker export. Some executions omit it.
    std::optional<Money> mAmount;
    // Positive fee paid; zero when the export has no fee.
    Money mFeePaid{};
    ExchangeRate mExchangeRate{EXCHANGE_RATE_SCALE};
    Currency mCurrency{Currency::Unknown};
    std::string mTransactionId;
};

struct TradeInstrument {
    std::string mName;
    Isin mIsin;
    AssetClass mAssetClass{AssetClass::Unknown};
    std::vector<TradeTransaction> mTransactions;
    std::vector<CorporateAction> mCorporateActions;
};

struct DividendTransaction {
    Date mDate{};
    Money mGrossAmount{};
    Money mTaxPaid{};
    ExchangeRate mExchangeRate{EXCHANGE_RATE_SCALE};
    Currency mCurrency{Currency::EUR};
    Currency mTaxCurrency{Currency::EUR};
    std::string mTransactionId;
};

struct DividendInstrument {
    std::string mName;
    Isin mIsin;
    std::vector<DividendTransaction> mTransactions;
};

struct InterestTransaction {
    Date mDate{};
    Money mGrossAmount{};
    Money mTaxPaid{};
    ExchangeRate mExchangeRate{EXCHANGE_RATE_SCALE};
    Currency mCurrency{Currency::EUR};
    Currency mTaxCurrency{Currency::EUR};
    std::string mTransactionId;
};

struct InterestInstrument {
    std::string mName;
    std::optional<Isin> mIsin;
    InterestType mInterestType{InterestType::UnknownInterest};
    std::vector<InterestTransaction> mTransactions;
};

// Broker benefits are preserved separately from security acquisitions. This keeps the exact
// credited amount available for local analytics without creating a duplicate buy transaction.
struct BenefitEvent {
    Date mDate{};
    BenefitType mType{};
    std::string mName;
    std::optional<Isin> mIsin;
    AssetClass mAssetClass{AssetClass::Unknown};
    Money mAmount{};
    Currency mCurrency{Currency::EUR};
    std::string mTransactionId;
};

struct PrivateMarketEvent {
    Date mDate{};
    PrivateMarketEventType mType{};
    std::string mName;
    std::optional<Isin> mIsin;
    AssetClass mAssetClass{AssetClass::Unknown};
    // Signed cash movement as exported by the broker.
    Money mAmount{};
    // Positive fee paid; zero when the export has no fee.
    Money mFeePaid{};
    Currency mCurrency{Currency::EUR};
    std::string mDescription;
    std::string mTransactionId;
};

struct BrokerStatement {
    std::vector<TradeInstrument> mTradeInstruments;
    std::vector<DividendInstrument> mDividendInstruments;
    std::vector<InterestInstrument> mInterestInstruments;
    std::vector<BenefitEvent> mBenefitEvents;
    std::vector<PrivateMarketEvent> mPrivateMarketEvents;
};

// Broker data with structured diagnostics used throughout the processing
// pipeline and by the frontend API.
struct ParseResult {
    Broker mBroker{Broker::Unknown};
    BrokerStatement mStatement;
    std::vector<ParseDiagnostic> mDiagnostics;
};

} // namespace taxbroker
