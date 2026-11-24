#pragma once

#include "taxbroker/types.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Checks that every parser's fuzz target and unit tests share. They read only the broker-neutral
// result, so they hold for any broker. The rows to check (which rows of an input must end as an
// event or an error) are per broker: see trade_republic_rows_to_check.hpp and
// ibkr_rows_to_check.hpp.

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
    detail::collectRows(aResult.mStatement.mBondRedemptions, eventRows);
    detail::collectRows(aResult.mStatement.mIncomePaymentRows, eventRows);
    detail::collectRows(aResult.mStatement.mLeftOutRows, eventRows);

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
