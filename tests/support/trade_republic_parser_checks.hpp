#pragma once

#include "taxbroker/types.hpp"

#include <csv.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace taxbroker::test {

/// Returns the first accepted trade whose sign disagrees with its side, or nothing. An accepted
/// trade stores positive units and a positive amount; its unit evidence (required) keeps the
/// source sign, which is negative exactly for a sale.
inline std::optional<std::string> tradeSignFailure(const ParseResult& aResult) {
    for (const auto& instrument : aResult.mStatement.mTradeInstruments)
    {
        for (const auto& trade : instrument.mTransactions)
        {
            const auto row = trade.mMetadata.mSources.empty()
                                 ? std::size_t{}
                                 : trade.mMetadata.mSources.front().mSourceRow;

            if (trade.mUnits <= 0)
            {
                return "row " + std::to_string(row) + ": an accepted trade has no positive units";
            }

            if (trade.mAmount && *trade.mAmount <= 0)
            {
                return "row " + std::to_string(row) + ": an accepted trade has no positive amount";
            }

            if (trade.mUnitEvidence.empty())
            {
                return "row " + std::to_string(row) + ": an accepted trade has no unit evidence";
            }

            for (const auto& evidence : trade.mUnitEvidence)
            {
                const bool isNegative = evidence.mCanonicalValue.starts_with('-');

                if (isNegative != (trade.mTradeSide == TradeSide::Sell))
                {
                    return "row " + std::to_string(row) +
                           ": the source sign of the units does not match the trade side";
                }
            }
        }
    }

    return std::nullopt;
}

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
/// out. Gives an empty list when the file has no category or type column or cannot be read.
inline std::vector<std::size_t> recordRowsToCheck(const std::filesystem::path& aPath) {
    std::vector<std::size_t> rows;

    try
    {
        csv::CSVFormat format;
        format.delimiter(',').variable_columns(csv::VariableColumnPolicy::KEEP);
        csv::CSVReader reader(aPath.string(), format);

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

namespace detail {

template <typename Event>
void collectRows(const std::vector<Event>& aEvents, std::set<std::size_t>& aRows) {
    for (const auto& event : aEvents)
    {
        for (const auto& source : event.mMetadata.mSources)
        {
            aRows.insert(source.mSourceRow);
        }
    }
}

} // namespace detail

/// Returns the first record row that has neither an event nor an error diagnostic, or nothing.
/// aRecordRows lists the logical rows (the header is row 1) that must not vanish; rows ignored on
/// purpose are left out by the caller. A file-level error (an error without a row) gives up the
/// row check, because the parser then discards the rows.
inline std::optional<std::string> droppedRowFailure(const ParseResult& aResult,
                                                    const std::vector<std::size_t>& aRecordRows) {
    std::set<std::size_t> eventRows;
    std::set<std::size_t> errorRows;

    for (const auto& instrument : aResult.mStatement.mTradeInstruments)
    {
        detail::collectRows(instrument.mTransactions, eventRows);
        detail::collectRows(instrument.mCorporateActions, eventRows);
    }

    for (const auto& instrument : aResult.mStatement.mDividendInstruments)
    {
        detail::collectRows(instrument.mTransactions, eventRows);
    }

    for (const auto& instrument : aResult.mStatement.mInterestInstruments)
    {
        detail::collectRows(instrument.mTransactions, eventRows);
    }

    detail::collectRows(aResult.mStatement.mBenefitEvents, eventRows);
    detail::collectRows(aResult.mStatement.mPrivateMarketEvents, eventRows);

    for (const auto& diagnostic : aResult.mDiagnostics)
    {
        if (diagnostic.mSeverity != DiagnosticSeverity::Error)
        {
            continue;
        }

        if (!diagnostic.mRowIndex)
        {
            return std::nullopt;
        }

        errorRows.insert(*diagnostic.mRowIndex);
    }

    const auto dropped =
        std::find_if(aRecordRows.begin(),
                     aRecordRows.end(),
                     [&eventRows, &errorRows](std::size_t aRow) {
                         return !eventRows.contains(aRow) && !errorRows.contains(aRow);
                     });

    if (dropped != aRecordRows.end())
    {
        return "row " + std::to_string(*dropped) + " has neither an event nor an error";
    }

    return std::nullopt;
}

} // namespace taxbroker::test
