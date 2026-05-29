#pragma once

#include "taxbroker/types.hpp"

#include <optional>
namespace taxbroker {

enum class RowType {
    Trade,
    Dividend,
    Interest,
    CorporateAction,
    Unknown
};

struct RowParsedValues {
    std::string mCategory;
    std::string mType;
};

struct RowMeta {
    RowType mRowType{RowType::Unknown};
    RowParsedValues mParsedValues;
};

struct GetAmount {
    std::optional<Money> mGrossAmount;
    std::optional<Money> mExchangeRate;
    std::optional<Currency> mCurrency;
};
} // namespace taxbroker