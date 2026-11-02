#pragma once

#include "taxbroker/types.hpp"

#include <optional>
#include <string_view>

namespace taxbroker {

/// The three-letter code of a supported currency, such as "NOK". Empty for Currency::Unknown.
[[nodiscard]] std::string_view currencyCode(Currency aCurrency) noexcept;

/// The currency of an exact uppercase code, such as "NOK". Currency::Unknown for any other text,
/// including a lowercase code and a code without an official ECB rate.
[[nodiscard]] Currency parseCurrencyCode(std::string_view aCode) noexcept;

/// The first day on which the euro replaced the currency (HRK 2023-01-01, BGN 2026-01-01), or
/// nothing for a currency that was not replaced. The ECB publishes no rate from that day on.
[[nodiscard]] std::optional<Date> euroReplacementDate(Currency aCurrency) noexcept;

} // namespace taxbroker
