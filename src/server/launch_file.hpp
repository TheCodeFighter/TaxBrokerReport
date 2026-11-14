#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace taxbroker {

/// The launch page: a redirect to http://127.0.0.1:<aPort>/#token=<aLinkToken> without a referrer,
/// and a visible link. Throws `std::invalid_argument` unless aLinkToken is 64 lower-case hex.
[[nodiscard]] std::string makeLaunchPage(std::uint16_t aPort, std::string_view aLinkToken);

/// Writes aContent to aPath readable by the user only (POSIX mode 0600, also when the file existed;
/// a symbolic link at aPath is refused). Throws `std::runtime_error` without the path in the text.
void writeLaunchFile(const std::filesystem::path& aPath, std::string_view aContent);

} // namespace taxbroker
