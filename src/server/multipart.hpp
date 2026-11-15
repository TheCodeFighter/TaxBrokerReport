#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace taxbroker {

/// The most parts one upload may have: the `request` part and 32 files.
inline constexpr std::size_t kMaxMultipartParts = 33;

/// One part of a `multipart/form-data` body.
struct MultipartPart {
    std::string mName;                    ///< The `name` of the Content-Disposition header.
    std::optional<std::string> mFileName; ///< The raw `filename`; none for a field.
    std::string_view mContent;            ///< A view into the body; the body must outlive it.
};

/// Why a body is not a usable `multipart/form-data` body.
enum class MultipartError {
    BadContentType, ///< Not `multipart/form-data`, or no valid boundary in the Content-Type.
    Malformed,      ///< The body breaks RFC 7578: no first delimiter, no closing line, ...
    BadPartHeader,  ///< A part header without a line end or colon, not `form-data`, or no name.
    TooManyParts,   ///< More than `kMaxMultipartParts` parts.
    TooLarge,       ///< A file part is longer than the limit.
};

/// A fixed word for the log and the bug report: `content_type`, `body`, `part_header`,
/// `too_many_parts` or `too_large`. It never holds a value of the request.
[[nodiscard]] std::string_view multipartErrorReason(MultipartError aError);

/// Splits a `multipart/form-data` body into its parts (RFC 7578): the boundary comes from the
/// Content-Type header, lines end with CRLF, text before the first and after the last boundary is
/// ignored. A file part (one with a `filename`) over aMaxFileBytes is `TooLarge`. The reader does
/// not look at the content of a part.
[[nodiscard]] std::variant<std::vector<MultipartPart>, MultipartError>
readMultipart(std::string_view aBody, std::string_view aContentType, std::size_t aMaxFileBytes);

} // namespace taxbroker
