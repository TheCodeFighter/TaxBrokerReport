#pragma once

#include "taxbroker/diagnostic_messages.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace taxbroker {

/// Whether a reference file ships with the app or was supplied by the user.
enum class CatalogueOrigin {
    BuiltIn,
    Supplied,
};

/// Where a reference row comes from. `mFile` is a file name without a path.
struct CatalogueSource {
    std::string mSource;         ///< "ib-edavki" or "manual".
    std::string mSourceRevision; ///< The upstream commit; empty for a "manual" row.
    std::string mFile;           ///< For example "payers.csv".
    std::size_t mRowNumber = 0;  ///< Logical row of the file, header = 1.
    CatalogueOrigin mOrigin{};   ///< Where the file came from.

    bool operator==(const CatalogueSource&) const = default;
};

/// The first fault of a reference file, with no value of the file in it.
struct ReferenceFileFault {
    std::string mFile;
    std::optional<std::size_t> mRow; ///< Logical row, header = 1; none for the whole file.
    DiagnosticMessage mMessage;
};

} // namespace taxbroker
