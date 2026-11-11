#pragma once

#include <string_view>

namespace taxbroker {

/// Whether the text is a Slovenian tax number: exactly eight digits whose last digit is the check
/// digit of the first seven (weights 8 to 2, 11 minus the sum modulo 11, 10 becomes 0). A sum that
/// is a multiple of 11 has no check digit, so such a number is not valid.
[[nodiscard]] bool isSlovenianTaxNumber(std::string_view aText);

} // namespace taxbroker
