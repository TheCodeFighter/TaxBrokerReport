#pragma once

#include <string>

namespace taxbroker {

struct ParseResult;

namespace api {

inline constexpr unsigned kDiagnosticsSchemaVersion = 1;

// Produces the diagnostics object that the future HTTP API can return directly. A negative indent
// creates compact JSON; a non-negative indent is useful for local debug artifacts.
[[nodiscard]] std::string serializeDiagnosticsJson(const ParseResult& aParseResult,
                                                   int aIndent = -1);

} // namespace api

} // namespace taxbroker
