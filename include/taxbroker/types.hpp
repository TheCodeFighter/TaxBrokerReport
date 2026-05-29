#pragma once

#include "taxbroker/errors.hpp"

#include <cstdint>
#include <chrono>
#include <string>
#include <ratio>
#include <vector>
#include <optional>

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

struct CorporateAction {
    Date mDate{};
    CorporateActionType mType{};
    CorpRatio mRatio{CORP_RATIO_SCALE};
};

struct TradeTransaction {
    Date mDate{};
    TradeSide mTradeSide{TradeSide::Buy};
    Money mUnitPrice{};
    Units mUnits{};
    Currency mCurrency{Currency::EUR};
};

struct TradeInstrument {
    std::string mName;
    Isin mIsin;
    std::vector<TradeTransaction> mTransactions;
    std::vector<CorporateAction>
        mCorporateActions; // Optional corporate actions affecting transaction history.
};

struct DividendTransaction {
    Date mDate{};
    Money mGrossAmount{};
    Money mTaxPaid{};
    Currency mCurrency{Currency::EUR};
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
    Currency mCurrency{Currency::EUR};
};

struct InterestInstrument {
    std::string mName; // No ISIN for interest transactions, using name instead.
    std::optional<Isin>
        mIsin; // Optional ISIN if available, otherwise empty. (ie bonds has isin, broker not)
    InterestType mInterestType;
    std::vector<InterestTransaction> mTransactions;
};

struct BrokerStatement {
    std::vector<TradeInstrument> mTradeInstruments;
    std::vector<DividendInstrument> mDividendInstruments;
    std::vector<InterestInstrument> mInterestInstruments;
};

// Canonical parsed broker data with warnings used throughout the processing pipeline.
struct ParseResult {
    BrokerStatement mStatement;
    std::vector<ParseWarning> mWarnings;
};

} // namespace taxbroker
