#include "server/multipart.hpp"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

/// Stops the run on a failed check. libFuzzer saves the input that caused the trap.
[[noreturn]] void fail(const char* aMessage) {
    std::fprintf(stderr, "FUZZ CHECK FAILED: %s\n", aMessage);
    __builtin_trap();
}

constexpr std::size_t kFileLimit = 1024;

} // namespace

/// Turns logging off once, so the fuzzer output stays readable.
extern "C" int LLVMFuzzerInitialize(int*, char***) {
    spdlog::set_level(spdlog::level::off);

    return 0;
}

/// Reads one input as a `multipart/form-data` body with the boundary `XyZ` and stops the run when
/// a part leaves the body, a file is over the limit or there are too many parts.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* aData, std::size_t aSize) {
    const std::string_view body{reinterpret_cast<const char*>(aData), aSize};
    const auto result = readMultipart(body, "multipart/form-data; boundary=XyZ", kFileLimit);

    const auto* parts = std::get_if<std::vector<MultipartPart>>(&result);

    if (parts == nullptr)
    {
        return 0;
    }

    if (parts->size() > kMaxMultipartParts)
    {
        fail("more parts than allowed");
    }

    for (const MultipartPart& part : *parts)
    {
        const char* begin = part.mContent.data();

        if (!part.mContent.empty() &&
            (begin < body.data() || begin + part.mContent.size() > body.data() + body.size()))
        {
            fail("a part lies outside the body");
        }

        if (part.mFileName.has_value() && part.mContent.size() > kFileLimit)
        {
            fail("a file part over the limit was returned");
        }

        if (part.mName.empty())
        {
            fail("a part without a name was returned");
        }
    }

    return 0;
}
