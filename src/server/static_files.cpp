#include "static_files.hpp"

#include <array>
#include <cstddef>

namespace taxbroker {

namespace {

constexpr std::string_view kPlaceholderPage =
    "<!doctype html>\n"
    "<html lang=\"sl\"><head><meta charset=\"utf-8\"><title>TaxBrokerReport</title></head>\n"
    "<body><p>TaxBrokerReport teče. Uporabniški vmesnik pride v naslednji različici.</p>\n"
    "<p lang=\"en\">TaxBrokerReport is running. The user interface comes in a later "
    "version.</p></body></html>\n";

constexpr std::array<StaticFile, 1> kPlaceholderFiles{StaticFile{"index.html", kPlaceholderPage}};

} // namespace

#ifdef TAXBROKER_EMBED_FRONTEND
/// Defined by the source that CMake generates from the built frontend files.
std::span<const StaticFile> frontendStaticFiles();
#endif

std::string_view contentTypeFor(std::string_view aPath) {
    const std::size_t dot = aPath.rfind('.');
    const std::string_view extension =
        dot == std::string_view::npos ? std::string_view{} : aPath.substr(dot + 1);

    if (extension == "html")
    {
        return "text/html; charset=utf-8";
    }

    if (extension == "js")
    {
        return "text/javascript; charset=utf-8";
    }

    if (extension == "css")
    {
        return "text/css; charset=utf-8";
    }

    if (extension == "webp")
    {
        return "image/webp";
    }

    return "application/octet-stream";
}

std::span<const StaticFile> placeholderStaticFiles() {
    return kPlaceholderFiles;
}

std::span<const StaticFile> embeddedStaticFiles() {
#ifdef TAXBROKER_EMBED_FRONTEND
    return frontendStaticFiles();
#else
    return placeholderStaticFiles();
#endif
}

} // namespace taxbroker
