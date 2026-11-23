#pragma once

#include <string_view>

namespace taxbroker {

/// Whether the text is an officially assigned ISO 3166-1 alpha-2 country code in capital letters.
/// `XS`, `EU`, `XX` and `XK` are no country here: the first three are ISIN prefixes or placeholders
/// that eDavki rejects.
[[nodiscard]] bool isIsoCountryCode(std::string_view aCode);

} // namespace taxbroker
