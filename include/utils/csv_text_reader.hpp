#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace taxbroker {

/// What made the reader stop. The caller maps it to its own message.
enum class CsvFaultKind {
    UnclosedQuote,      ///< An odd number of quote characters in the text; the fault has no row.
    MissingColumns,     ///< Required columns are not in the header; `mColumns` lists them.
    RepeatedColumn,     ///< The header has a required column twice; `mColumns` has the first one.
    LineBreakInField,   ///< A field of the row holds a line break (a quote that closes later).
    FieldCountMismatch, ///< The row has another field count than the header.
    Unreadable,         ///< The CSV library could not read on after the rows returned; no row.
};

/// The first fault of a text. Only the members named for the kind are set.
struct CsvFault {
    CsvFaultKind mKind{};                  ///< What was wrong.
    std::optional<std::size_t> mRowNumber; ///< Logical row, header = 1; none for the whole text.
    std::vector<std::string> mColumns;     ///< MissingColumns (required order), RepeatedColumn.
    std::size_t mExpectedFields = 0;       ///< FieldCountMismatch: the header's field count.
    std::size_t mFoundFields = 0;          ///< FieldCountMismatch: the row's field count.
};

/// One record of the text.
struct CsvRow {
    std::size_t mRowNumber = 0; ///< Logical row, header = 1. A quoted line break stays one row.
    std::vector<std::string> mFields; ///< The fields without their quotes.
};

/// The result of `readCsvText`.
struct CsvTable {
    std::vector<std::string> mHeader; ///< The column names of row 1, in file order.
    std::vector<CsvRow> mRows;        ///< Only the rows before the fault.
    std::optional<CsvFault> mFault;   ///< The first fault, if any.

    /// Index of the first header column with this name. Throws std::logic_error when it is absent.
    [[nodiscard]] std::size_t columnIndex(std::string_view aName) const;
};

/// Reads CSV text with a header row and returns its rows up to the first fault. The checks run in
/// this order: unclosed quote, missing required columns, repeated required column, then for each
/// row a line break inside a field and the field count. Input the CSV library cannot read on, such
/// as a row longer than two 10 MB chunks, ends the rows with an Unreadable fault. A header row
/// longer than one 10 MB chunk gives an empty header, so a MissingColumns fault.
[[nodiscard]] CsvTable readCsvText(std::string_view aText,
                                   char aDelimiter,
                                   std::span<const std::string_view> aRequiredColumns);

} // namespace taxbroker
