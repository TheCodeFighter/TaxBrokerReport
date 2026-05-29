#pragma once

namespace taxbroker {

enum class RowType {
    Trade,
    Dividend,
    Cash,
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

} // namespace taxbroker