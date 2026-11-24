#pragma once

#include <csv.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace taxbroker::test::tr {

/// The cash movements the parser ignores on purpose: category CASH with one of these types. The
/// list is written out here on purpose, so a change in the parser's own list shows as a finding.
constexpr std::array kIgnoredCashTypes{
    std::string_view{"CARD_FAILED_TRANSACTION"},
    std::string_view{"CARD_ORDER_BILLED"},
    std::string_view{"CARD_ORDERING_FEE"},
    std::string_view{"CARD_TRANSACTION"},
    std::string_view{"CARD_TRANSACTION_INTERNATIONAL"},
    std::string_view{"CUSTOMER_INBOUND"},
    std::string_view{"CUSTOMER_INPAYMENT"},
    std::string_view{"CUSTOMER_OUTBOUND_REQUEST"},
    std::string_view{"TRANSFER_INSTANT_INBOUND"},
    std::string_view{"TRANSFER_INSTANT_OUTBOUND"},
    std::string_view{"TRANSFER_OUTBOUND"},
};

/// The logical rows (header = 1) that must end as an event or an error, read with the same CSV
/// format as the parser. A record with the header's field count and an ignored cash type is left
/// out. Gives an empty list when the text has no category or type column or cannot be read.
inline std::vector<std::size_t> recordRowsToCheck(std::string_view aText) {
    std::vector<std::size_t> rows;

    try
    {
        csv::CSVFormat format;
        format.delimiter(',').variable_columns(csv::VariableColumnPolicy::KEEP);
        std::stringstream stream{std::string{aText}};
        csv::CSVReader reader(stream, format);

        const auto names = reader.get_col_names();
        const auto hasColumn = [&names](std::string_view aName) {
            return std::find(names.begin(), names.end(), aName) != names.end();
        };

        if (!hasColumn("category") || !hasColumn("type"))
        {
            return rows;
        }

        std::size_t rowIndex = 1;

        for (const csv::CSVRow& row : reader)
        {
            ++rowIndex;

            if (row.size() == names.size())
            {
                const auto category = row["category"].get<std::string>();
                const auto type = row["type"].get<std::string>();
                const bool isIgnored =
                    category == "CASH" &&
                    std::find(kIgnoredCashTypes.begin(), kIgnoredCashTypes.end(), type) !=
                        kIgnoredCashTypes.end();

                if (isIgnored)
                {
                    continue;
                }
            }

            rows.push_back(rowIndex);
        }
    } catch (const std::exception&)
    { rows.clear(); }

    return rows;
}

/// The same for a file; an empty list when the file cannot be read.
inline std::vector<std::size_t> recordRowsToCheck(const std::filesystem::path& aPath) {
    std::ifstream input{aPath, std::ios::binary};

    if (!input)
    {
        return {};
    }

    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

    return recordRowsToCheck(std::string_view{text});
}

} // namespace taxbroker::test::tr
