#include "taxbroker/build_info.hpp"

#include "taxbroker/logging.hpp"

namespace taxbroker {

namespace {

constexpr std::string_view kProgramName = "TaxBrokerReport";

constexpr std::string_view kBuildType =
#ifdef NDEBUG
    "release";
#else
    "debug";
#endif

constexpr std::string_view kOperatingSystem =
#if defined(__linux__)
    "linux";
#elif defined(_WIN32)
    "windows";
#elif defined(__APPLE__)
        "macos";
#else
        "unknown";
#endif

constexpr std::string_view kProcessor =
#if defined(__x86_64__) || defined(_M_X64)
    "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    "aarch64";
#else
    "unknown";
#endif

} // namespace

std::string_view buildVersion() {
    return TAXBROKER_VERSION;
}

std::string buildInfoText() {
    std::string text{kProgramName};

    text += ' ';
    text += buildVersion();

    return text;
}

std::string buildPlatformText() {
    std::string text{kBuildType};

    text += ", ";
    text += kOperatingSystem;
    text += ' ';
    text += kProcessor;

    return text;
}

void logProgramStart(std::string_view aProgram) {
    LOG_INFO("{} started: {} ({})", buildInfoText(), aProgram, buildPlatformText());
}

} // namespace taxbroker
