#include "taxbroker/tax_number.hpp"

#include <cstddef>

namespace taxbroker {

bool isSlovenianTaxNumber(std::string_view aText) {
    constexpr std::size_t kDigits = 8;
    constexpr int kModulus = 11;
    constexpr int kFirstWeight = 8;

    if (aText.size() != kDigits)
    {
        return false;
    }

    int sum = 0;

    for (std::size_t position = 0; position < kDigits; ++position)
    {
        if (aText[position] < '0' || aText[position] > '9')
        {
            return false;
        }

        // The first seven digits have the weights 8, 7, ..., 2.
        if (position + 1 < kDigits)
        {
            sum += (aText[position] - '0') * (kFirstWeight - static_cast<int>(position));
        }
    }

    const int remainder = sum % kModulus;

    if (remainder == 0)
    {
        return false;
    }

    // 11 - 1 is 10, which is written as 0.
    const int checkDigit = (kModulus - remainder) % 10;

    return aText[kDigits - 1] - '0' == checkDigit;
}

} // namespace taxbroker
