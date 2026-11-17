#pragma once

#include <string>
#include <string_view>

namespace taxbroker {

/// The version of the program, for example `2.0.0`.
[[nodiscard]] std::string_view buildVersion();

/// The name and version, for example `TaxBrokerReport 2.0.0`.
[[nodiscard]] std::string buildInfoText();

/// The facts of the build that are known at compile time, for example
/// `release, linux x86_64`: build type, operating system and processor. Never a host name, a user
/// name or a kernel release.
[[nodiscard]] std::string buildPlatformText();

/// Writes the info line `TaxBrokerReport 2.0.0 started: report (release, linux x86_64)`.
/// `aProgram` is a fixed word such as `report` or `server`.
void logProgramStart(std::string_view aProgram);

} // namespace taxbroker
