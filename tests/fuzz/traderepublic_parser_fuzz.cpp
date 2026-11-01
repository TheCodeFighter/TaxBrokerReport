#include "../support/trade_republic_parser_checks.hpp"
#include "parsers/traderepublic_parser.hpp"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

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

/// Parses one input as a CSV file and stops the run when a trade has the wrong sign or a record
/// row has neither an event nor an error.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* aData, std::size_t aSize) {
    const auto path = std::filesystem::path{"/tmp"} /
                      ("tbr-fuzz-" + std::to_string(static_cast<long>(getpid())) + ".csv");

    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(aData), static_cast<std::streamsize>(aSize));
    }

    const auto result = tr::TradeRepublicParser{}.parse(path, 0);

    if (const auto message = test::tradeSignFailure(result))
    {
        fail(*message);
    }

    if (const auto message = test::droppedRowFailure(result, test::recordRowsToCheck(path)))
    {
        fail(*message);
    }

    return 0;
}
