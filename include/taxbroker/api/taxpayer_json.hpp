#pragma once

#include "taxbroker/report_models.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace taxbroker::api {

/// Why a taxpayer JSON was refused: one English sentence that names the key. It holds no value.
struct TaxpayerJsonError {
    std::string mMessage; ///< English sentence for the CLI; names the key, never a value.
    /// The fixed taxpayer key the error is about (`taxNumber`, `birthDate`, ...) for the browser
    /// form. Empty when the text is not JSON or not an object, and for a key the application does
    /// not know (the name is the sender's text, so it is never repeated).
    std::string mKey;

    bool operator==(const TaxpayerJsonError&) const = default;
};

/// Reads the taxpayer object of the taxpayer file and of the HTTP request: the tax number, the
/// residence and the optional address and contact fields. Unknown keys are refused.
[[nodiscard]] std::variant<TaxpayerMetadata, TaxpayerJsonError>
parseTaxpayerJson(std::string_view aText);

/// The longest saved profile that is read; a longer file is unreadable.
inline constexpr std::size_t kMaxProfileBytes = 64 * 1024;

/// What a saved taxpayer profile holds. A damaged part is left out and named, never refused as a
/// whole.
struct TaxpayerProfile {
    /// The valid text fields by key (`taxNumber`, `name`, `birthDate`, ...), without `isResident`.
    std::map<std::string, std::string> mFields;
    std::vector<std::string> mInvalidKeys; ///< Known keys whose value is not valid, in key order.
    bool mIsResident{};
    bool mUnreadable{};  ///< Longer than the limit, not JSON or not an object; no fields.
    bool mIgnoredKeys{}; ///< The file holds a key the application does not know.
};

/// Reads a saved profile leniently: each known key is checked like in `parseTaxpayerJson`, a bad
/// one is listed in `mInvalidKeys`, an unknown one sets `mIgnoredKeys`. A missing key is just
/// empty.
[[nodiscard]] TaxpayerProfile readTaxpayerProfile(std::string_view aText);

} // namespace taxbroker::api
