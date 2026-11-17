#pragma once

#include <string>

namespace taxbroker {

/// The kind of the exception that is being handled, for a log line: `out_of_memory` for
/// `std::bad_alloc`, the type name for another `std::exception` (for example
/// `std::runtime_error`), else `unknown`. Never the exception text, which can hold a path or a
/// value. Call it only inside a `catch` block.
[[nodiscard]] std::string currentExceptionKind();

} // namespace taxbroker
