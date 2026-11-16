#pragma once

#include <span>
#include <string_view>

namespace taxbroker {

/// One file that the server serves from memory. The path has no leading slash.
struct StaticFile {
    std::string_view mPath; ///< `index.html`, `assets/app.js`, ...; always set by the creator.
    std::string_view mBody; ///< The bytes of the file; always set by the creator.
};

/// The `Content-Type` of a file, from its extension: `html`, `js`, `css` and `webp`. Any other
/// extension is `application/octet-stream`.
[[nodiscard]] std::string_view contentTypeFor(std::string_view aPath);

/// A page that says only that the server runs. It is the whole table of a build without the
/// frontend.
[[nodiscard]] std::span<const StaticFile> placeholderStaticFiles();

/// The files of the frontend embedded at build time (`TAXBROKER_FRONTEND_DIR`), or the
/// placeholder table when the build has none.
[[nodiscard]] std::span<const StaticFile> embeddedStaticFiles();

} // namespace taxbroker
