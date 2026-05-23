#pragma once

namespace taxbroker {

enum class RowType {
    Trade,
    Dividend,
    Interest,
    CorporateAction,
    Unknown
};

} // namespace taxbroker