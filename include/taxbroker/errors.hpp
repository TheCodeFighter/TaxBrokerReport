#pragma once

#include <string>
#include <cstddef>

namespace taxbroker {

enum class WarningCode {
    UnsupportedRowType,
    MissingField,
    InvalidValue,
    ParseError
};

struct ParseWarning {
    WarningCode mCode{};
    std::string mSourceFile;
    std::size_t mRowIndex{};
    std::string mMessage;
};

} // namespace taxbroker