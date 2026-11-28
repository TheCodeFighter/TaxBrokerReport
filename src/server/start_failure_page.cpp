#include "start_failure_page.hpp"

#include "launch_file.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <string_view>
#include <system_error>
#include <vector>

namespace taxbroker {

namespace {

constexpr std::string_view kPageName = "start_failed.html";
constexpr std::string_view kMailAddress = "tax.brokerage.report@gmail.com";
constexpr std::string_view kIssuesLink = "https://github.com/TheCodeFighter/TaxBrokerReport/issues";

/// The text with the five HTML special characters replaced by their entities.
std::string escapeHtml(std::string_view aText) {
    std::string escaped;
    escaped.reserve(aText.size());

    for (const char character : aText)
    {
        switch (character)
        {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&#39;";
            break;
        default:
            escaped += character;
            break;
        }
    }

    return escaped;
}

/// The text for a `mailto:` link: every byte but a letter, a digit and `-._~` as `%XX`.
std::string percentEncode(std::string_view aText) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string encoded;

    for (const char character : aText)
    {
        const bool plain = (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '-' ||
                           character == '.' || character == '_' || character == '~';

        if (plain)
        {
            encoded += character;

            continue;
        }

        const auto byte = static_cast<unsigned char>(character);
        encoded += '%';
        encoded += kHex[byte >> 4U];
        encoded += kHex[byte & 0x0FU];
    }

    return encoded;
}

/// The folders for the page in order: the launch folder (when `XDG_RUNTIME_DIR` is absolute),
/// then the state folder. The state folder may throw when there is none.
std::vector<std::filesystem::path> pageFolders(const EnvLookup& aEnv) {
    std::vector<std::filesystem::path> folders;

    const std::optional<std::filesystem::path> runtimeDir =
        absoluteEnvironmentPath(aEnv, "XDG_RUNTIME_DIR");

    if (runtimeDir.has_value())
    {
        folders.push_back(*runtimeDir / "taxbroker");
    }

    try
    { folders.push_back(stateFolder(aEnv)); } catch (const std::exception&)
    {
        // No state folder: only the launch folder is left.
    }

    return folders;
}

} // namespace

std::string makeStartFailurePage(std::string_view aError, std::string_view aVersion) {
    const std::string error = escapeHtml(aError);
    const std::string version = escapeHtml(aVersion);
    const std::string subject =
        percentEncode("TaxBrokerReport " + std::string(aVersion) + " did not start");
    const std::string mailto = "mailto:" + std::string(kMailAddress) + "?subject=" + subject +
                               "&amp;body=" + percentEncode(aError);

    return "<!doctype html>\n"
           "<html lang=\"sl\"><head><meta charset=\"utf-8\">"
           "<meta name=\"referrer\" content=\"no-referrer\">\n"
           "<title>TaxBrokerReport se ni zagnal / did not start</title>\n"
           "<style>body{font-family:sans-serif;max-width:42em;margin:2em auto;padding:0 1em;"
           "line-height:1.5}code{background:#eee;padding:0 .3em}small{color:#555}</style>"
           "</head>\n<body>\n"
           "<h1>TaxBrokerReport se ni zagnal</h1>\n"
           "<p>Napaka: <code>" +
           error +
           "</code></p>\n"
           "<p>Program zaženite znova. Če se spet ne zažene, pošljite datoteko dnevnika "
           "<code>~/.local/state/taxbroker/taxbroker.log</code> na <a href=\"" +
           mailto + "\">" + std::string(kMailAddress) + "</a> ali odprite prijavo na <a href=\"" +
           std::string(kIssuesLink) +
           "\">GitHubu</a>.</p>\n"
           "<p><small>Različica " +
           version +
           "</small></p>\n"
           "<hr>\n"
           "<h1>TaxBrokerReport did not start</h1>\n"
           "<p>Error: <code>" +
           error +
           "</code></p>\n"
           "<p>Start the program again. If it fails again, send the log file "
           "<code>~/.local/state/taxbroker/taxbroker.log</code> to <a href=\"" +
           mailto + "\">" + std::string(kMailAddress) + "</a> or open an issue on <a href=\"" +
           std::string(kIssuesLink) +
           "\">GitHub</a>.</p>\n"
           "<p><small>Version " +
           version +
           "</small></p>\n"
           "<hr>\n"
           "<p><small>Ni davčno svetovanje in ni povezano s FURS ali s posrednikom; vrednosti in "
           "datoteke preverite in napoved oddate sami. Program je na voljo »tak, kot je«, brez "
           "jamstva; razvijalec ne odgovarja za davčne napake, globe ali opustitve.<br>"
           "Not tax advice and not affiliated with FURS or any broker; you check every value and "
           "file the return yourself. Provided as is, without warranty; no liability for tax "
           "errors, fines or omissions.</small></p>\n"
           "</body></html>\n";
}

std::optional<std::filesystem::path>
writeStartFailurePage(const EnvLookup& aEnv, std::string_view aError, std::string_view aVersion) {
    const std::string page = makeStartFailurePage(aError, aVersion);

    // The launch folder may be the very reason the start failed, so the state folder is the
    // second try.
    for (const std::filesystem::path& folder : pageFolders(aEnv))
    {
        try
        {
            makePrivateFolder(folder);

            const std::filesystem::path file = folder / kPageName;
            writeLaunchFile(file, page);

            return file;
        } catch (const std::exception&)
        {
            // This folder or file does not work: try the next one.
        }
    }

    // No folder or no file: the error is on stderr and in the log as before.
    return std::nullopt;
}

void removeStartFailurePage(const EnvLookup& aEnv) {
    for (const std::filesystem::path& folder : pageFolders(aEnv))
    {
        std::error_code ignored;
        std::filesystem::remove(folder / kPageName, ignored);
    }
}

} // namespace taxbroker
