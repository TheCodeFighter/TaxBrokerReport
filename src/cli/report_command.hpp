#pragma once

#include <istream>
#include <ostream>
#include <span>
#include <string>

namespace taxbroker::cli {

/// The XML was written, or no sale of the year needs a Doh-KDVP form.
inline constexpr int kExitSuccess = 0;
/// The capital report is blocked by at least one error; no XML was written.
inline constexpr int kExitBlocked = 1;
/// The command could not run (options, files, output folder); no output file was changed.
inline constexpr int kExitFailure = 2;

/// Runs the `taxbroker` command line without the program name. The answers to the questions of
/// `report` are read from aIn.
int runCli(std::span<const std::string> aArguments,
           std::istream& aIn,
           std::ostream& aOut,
           std::ostream& aErr);

} // namespace taxbroker::cli
