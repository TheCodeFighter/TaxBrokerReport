#pragma once

#include <string_view>

namespace taxbroker::test {

/// Sets an environment variable of this process. The value is UTF-8 text. On Windows a value
/// that is empty removes the variable (`_wputenv_s` does that).
void setTestEnvironment(std::string_view aName, std::string_view aValue);

/// Removes an environment variable of this process. A variable that is not set is fine.
void unsetTestEnvironment(std::string_view aName);

} // namespace taxbroker::test
