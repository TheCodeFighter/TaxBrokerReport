#include "../support/ibkr_rows_to_check.hpp"
#include "../support/parser_checks.hpp"
#include "parsers/ibkr_parser.hpp"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {
using namespace taxbroker;

/// Stops the run on a failed check. libFuzzer saves the input that caused the trap.
[[noreturn]] void fail(const std::string& aMessage) {
    std::fprintf(stderr, "FUZZ CHECK FAILED: %s\n", aMessage.c_str());
    __builtin_trap();
}

} // namespace

/// Turns logging off once, so the parser's error log does not flood the fuzzer output.
extern "C" int LLVMFuzzerInitialize(int*, char***) {
    spdlog::set_level(spdlog::level::off);

    return 0;
}

/// Parses one input as Flex Query XML and stops the run when a trade has the wrong sign or a row
/// has neither an event nor an error.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* aData, std::size_t aSize) {
    const std::string_view text{reinterpret_cast<const char*>(aData), aSize};
    const auto result = ibkr::IbkrParser{}.parse(text, "fuzz.xml", 0);

    if (const auto message = test::tradeSignFailure(result))
    {
        fail(*message);
    }

    if (const auto message = test::droppedRowFailure(result, test::ibkr::rowsToCheck(text)))
    {
        fail(*message);
    }

    return 0;
}
