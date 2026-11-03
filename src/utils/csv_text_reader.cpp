#include "utils/csv_text_reader.hpp"

#include <csv.hpp>

#include <algorithm>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace taxbroker {

namespace {

/// An odd number of quote characters means a quoted field never closes, and the reader would
/// merge the following records into it without any error.
bool hasUnclosedQuote(std::string_view aText) {
    return std::count(aText.begin(), aText.end(), '"') % 2 != 0;
}

/// A line break inside a field means a quote opened on one line and closed on a later one. The
/// reader then merges the lines into one record, which can still have the right field count.
bool hasLineBreakInsideField(const csv::CSVRow& aRow) {
    return std::any_of(aRow.begin(), aRow.end(), [](const csv::CSVField& aField) {
        return aField.get_sv().find_first_of("\r\n") != std::string_view::npos;
    });
}

} // namespace

std::size_t CsvTable::columnIndex(std::string_view aName) const {
    const auto found = std::find(mHeader.begin(), mHeader.end(), aName);

    if (found == mHeader.end())
    {
        throw std::logic_error{"The CSV table has no column " + std::string{aName}};
    }

    return static_cast<std::size_t>(found - mHeader.begin());
}

CsvTable readCsvText(std::string_view aText,
                     char aDelimiter,
                     std::span<const std::string_view> aRequiredColumns) {
    CsvTable table;

    if (hasUnclosedQuote(aText))
    {
        table.mFault = CsvFault{.mKind = CsvFaultKind::UnclosedQuote};

        return table;
    }

    // KEEP delivers every record. The default silently drops records whose field count differs
    // from the header.
    csv::CSVFormat format;

    format.delimiter(aDelimiter).variable_columns(csv::VariableColumnPolicy::KEEP);

    // Not csv::parse: its reader keeps a reference to a stream that ends when csv::parse returns,
    // reads it again after the first 10 MB chunk, and reads a string_view's data up to a zero byte
    // instead of its size. This stream lives as long as the reader.
    std::stringstream stream{std::string{aText}};
    csv::CSVReader reader{stream, format};

    table.mHeader = reader.get_col_names();

    std::vector<std::string_view> missingViews;

    std::copy_if(aRequiredColumns.begin(),
                 aRequiredColumns.end(),
                 std::back_inserter(missingViews),
                 [&table](std::string_view aColumn) {
                     return std::find(table.mHeader.begin(), table.mHeader.end(), aColumn) ==
                            table.mHeader.end();
                 });

    if (!missingViews.empty())
    {
        std::vector<std::string> missingColumns(missingViews.begin(), missingViews.end());

        table.mFault =
            CsvFault{.mKind = CsvFaultKind::MissingColumns, .mColumns = std::move(missingColumns)};

        return table;
    }

    // Two columns with one name leave it unclear which one holds the value.
    const auto repeatedColumn =
        std::find_if(aRequiredColumns.begin(),
                     aRequiredColumns.end(),
                     [&table](std::string_view aColumn) {
                         return std::count(table.mHeader.begin(), table.mHeader.end(), aColumn) > 1;
                     });

    if (repeatedColumn != aRequiredColumns.end())
    {
        table.mFault = CsvFault{.mKind = CsvFaultKind::RepeatedColumn,
                                .mColumns = {std::string{*repeatedColumn}}};

        return table;
    }

    // The library hands over the rows it has read and rethrows its worker error only when it runs
    // out of them, so the rows before an unreadable part are kept.
    try
    {
        std::size_t rowNumber = 1;

        for (const csv::CSVRow& row : reader)
        {
            ++rowNumber;

            if (hasLineBreakInsideField(row))
            {
                table.mFault =
                    CsvFault{.mKind = CsvFaultKind::LineBreakInField, .mRowNumber = rowNumber};

                return table;
            }

            if (row.size() != table.mHeader.size())
            {
                table.mFault = CsvFault{.mKind = CsvFaultKind::FieldCountMismatch,
                                        .mRowNumber = rowNumber,
                                        .mExpectedFields = table.mHeader.size(),
                                        .mFoundFields = row.size()};

                return table;
            }

            CsvRow parsed{.mRowNumber = rowNumber};

            for (const csv::CSVField& field : row)
            {
                parsed.mFields.emplace_back(field.get_sv());
            }

            table.mRows.push_back(std::move(parsed));
        }
    } catch (const std::runtime_error&)
    { table.mFault = CsvFault{.mKind = CsvFaultKind::Unreadable}; }

    return table;
}

} // namespace taxbroker
