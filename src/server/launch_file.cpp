#include "taxbroker/server/launch_file.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

#if defined(_WIN32)
#include <fstream>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace taxbroker {

namespace {

constexpr std::size_t kLinkTokenLength = 64;

/// True for 0-9 and a-f.
bool isLowerCaseHex(char aCharacter) {
    return (aCharacter >= '0' && aCharacter <= '9') || (aCharacter >= 'a' && aCharacter <= 'f');
}

[[noreturn]] void failWrite() {
    throw std::runtime_error("cannot write the launch file");
}

} // namespace

std::string makeLaunchPage(std::uint16_t aPort, std::string_view aLinkToken) {
    if (aLinkToken.size() != kLinkTokenLength ||
        !std::all_of(aLinkToken.begin(), aLinkToken.end(), isLowerCaseHex))
    {
        throw std::invalid_argument("the link token must be 64 lower-case hex characters");
    }

    const std::string link =
        "http://127.0.0.1:" + std::to_string(aPort) + "/#token=" + std::string(aLinkToken);

    return "<!doctype html>\n"
           "<html><head><meta charset=\"utf-8\"><meta name=\"referrer\" content=\"no-referrer\">\n"
           "<meta http-equiv=\"refresh\" content=\"0;url=" +
           link +
           "\">\n"
           "<title>TaxBrokerReport</title></head>\n"
           "<body><a href=\"" +
           link + "\">TaxBrokerReport</a></body></html>\n";
}

#if defined(_WIN32)

void writeLaunchFile(const std::filesystem::path& aPath, std::string_view aContent) {
    // The folder is the user's %LOCALAPPDATA%, whose access list allows only that user.
    std::ofstream file(aPath, std::ios::binary | std::ios::trunc);

    if (!file)
    {
        failWrite();
    }

    file.write(aContent.data(), static_cast<std::streamsize>(aContent.size()));
    file.close();

    if (!file)
    {
        failWrite();
    }
}

#else

void writeLaunchFile(const std::filesystem::path& aPath, std::string_view aContent) {
    const int descriptor =
        ::open(aPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);

    if (descriptor < 0)
    {
        failWrite();
    }

    // An existing file keeps its old mode in open(), so set it again.
    bool succeeded = ::fchmod(descriptor, 0600) == 0;

    std::size_t written = 0;

    while (succeeded && written < aContent.size())
    {
        const ssize_t count =
            ::write(descriptor, aContent.data() + written, aContent.size() - written);

        if (count <= 0)
        {
            // Retry only when a signal interrupted the call.
            succeeded = count < 0 && errno == EINTR;
            continue;
        }

        written += static_cast<std::size_t>(count);
    }

    succeeded = ::close(descriptor) == 0 && succeeded;

    if (!succeeded)
    {
        failWrite();
    }
}

#endif

} // namespace taxbroker
