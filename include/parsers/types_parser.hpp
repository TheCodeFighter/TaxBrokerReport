#pragma once

#include "taxbroker/types.hpp"

#include <optional>
#include <string>

namespace taxbroker {

enum class RowType {
    Trade,
    Dividend,
    Interest,
    CorporateAction,
    Benefit,
    PrivateMarket,
    Ignored,
    Unsupported,
    Unknown
};

struct RowParsedValues {
    std::string mCategory;
    std::string mType;
    std::string mAssetClass;
    std::string mTransactionId;
};

struct RowMeta {
    RowType mRowType{RowType::Unknown};
    RowParsedValues mParsedValues;
};

struct GetAmount {
    std::optional<Money> mGrossAmount;
    std::optional<ExchangeRate> mExchangeRate;
    std::optional<Currency> mCurrency;
};
} // namespace taxbroker
