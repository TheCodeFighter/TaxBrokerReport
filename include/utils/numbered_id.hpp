#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace taxbroker {

/// Pads a number to at least four digits: `diagnostic-0001`, `source-0002`, `decision-10000`.
[[nodiscard]] inline std::string numberedId(std::string_view aPrefix, std::size_t aNumber) {
    auto digits = std::to_string(aNumber);

    if (digits.size() < 4)
    {
        digits.insert(0, 4 - digits.size(), '0');
    }

    return std::string{aPrefix} + "-" + digits;
}

} // namespace taxbroker
