#pragma once

#include "taxbroker/application_request.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace taxbroker::api {

/// The most files one upload may name.
inline constexpr std::size_t kMaxUploadFiles = 32;

/// One part of the upload as the request reader sees it. The content is a view into the body.
struct UploadedPart {
    std::string mName;                    ///< The form field name: `request`, `file0`, ...
    std::optional<std::string> mFileName; ///< The raw file name the browser sent; none for a field.
    std::string_view mContent;            ///< The bytes of the part.
};

/// Why an upload is not a usable run request. Each value has a stable code for the frontend.
enum class RunRequestError {
    BadRequest,        ///< Not JSON, a wrong field type or an unknown field.
    PathNotAllowed,    ///< A key named `path` anywhere in the request.
    NoBrokerFile,      ///< No broker file in the upload.
    MissingPart,       ///< A named part is not in the upload.
    UnreferencedPart,  ///< A part that the request does not name.
    DuplicatePart,     ///< A part name used twice.
    TooManyFiles,      ///< More than `kMaxUploadFiles` files in the list.
    UnsupportedBroker, ///< A broker the application does not read.
    BadTaxpayer,       ///< The taxpayer object is refused; the refusal can name the key.
};

/// A refused upload: the error and, for `BadTaxpayer`, the fixed taxpayer key it is about (empty
/// when the key is unknown or the error is another one).
struct RunRequestRefusal {
    RunRequestError mError; ///< Why the upload is refused.
    std::string mField;     ///< The taxpayer key for `BadTaxpayer`; empty otherwise or unknown.

    explicit RunRequestRefusal(RunRequestError aError, std::string aField = {})
        : mError(aError), mField(std::move(aField)) {}
};

/// The code of an error as the frontend gets it.
[[nodiscard]] std::string_view runRequestErrorCode(RunRequestError aError);

/// Builds the application request from the parts of an upload: the part `request` holds the JSON
/// (year, filing type, developer mode, taxpayer and the list of files) and every other part is a
/// file that the list names.
[[nodiscard]] std::variant<ApplicationRequest, RunRequestRefusal>
buildApplicationRequest(std::span<const UploadedPart> aParts);

} // namespace taxbroker::api
