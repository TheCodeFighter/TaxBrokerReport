#pragma once

#include <string>
#include <string_view>

namespace taxbroker {

/// The SHA-256 hash (FIPS 180-4) of the bytes, as 64 lowercase hexadecimal digits.
[[nodiscard]] std::string sha256Hex(std::string_view aBytes);

} // namespace taxbroker
