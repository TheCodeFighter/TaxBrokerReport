#pragma once

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace taxbroker::test::ibkr {

/// The cash types the parser ignores on purpose (not income). The list is written out here on
/// purpose, so a change in the parser's own list shows as a finding.
constexpr std::array kIgnoredCashTypes{
    std::string_view{"Bond Interest Paid"},
    std::string_view{"Other Fees"},
    std::string_view{"Commission Adjustments"},
    std::string_view{"Advisor Fees"},
    std::string_view{"Deposits/Withdrawals"},
};

/// The corporate action types the parser ignores on purpose: the cash row is the payment.
constexpr std::array kIgnoredActionTypes{
    std::string_view{"CD"},
    std::string_view{"CP"},
};

namespace detail {

/// The offsets at which each line starts, built once per text.
inline std::vector<std::size_t> lineStartsOf(std::string_view aText) {
    std::vector<std::size_t> starts{0};

    for (std::size_t position = 0; position < aText.size(); ++position)
    {
        if (aText[position] == '\n')
        {
            starts.push_back(position + 1);
        }
    }

    return starts;
}

/// The line of the row's start tag (1-based), counted as the parser counts it; 0 when unknown.
inline std::size_t lineOf(const pugi::xml_node& aRow, const std::vector<std::size_t>& aLineStarts) {
    const auto offset = aRow.offset_debug();

    if (offset < 0)
    {
        return 0;
    }

    const auto next =
        std::upper_bound(aLineStarts.begin(), aLineStarts.end(), static_cast<std::size_t>(offset));

    return static_cast<std::size_t>(next - aLineStarts.begin());
}

inline bool hasAttribute(const pugi::xml_node& aRow, const char* aName) {
    return !aRow.attribute(aName).empty();
}

inline std::string_view valueOf(const pugi::xml_node& aRow, const char* aName) {
    return aRow.attribute(aName).value();
}

/// A `Trade` the parser leaves out on purpose: not an execution (counted in one notice), a
/// currency conversion (counted in one notice) or crypto without an ISIN (a notice on the row).
inline bool isIgnoredTrade(const pugi::xml_node& aRow) {
    if (!hasAttribute(aRow, "levelOfDetail") || !hasAttribute(aRow, "transactionID") ||
        !hasAttribute(aRow, "assetCategory"))
    {
        return false;
    }

    const auto level = valueOf(aRow, "levelOfDetail");

    // An empty level is rejected.
    if (level.empty())
    {
        return false;
    }

    if (level != "EXECUTION")
    {
        return true;
    }

    const auto category = valueOf(aRow, "assetCategory");

    return category == "CASH" || (category == "CRYPTO" && valueOf(aRow, "isin").empty());
}

/// A `CashTransaction` the parser leaves out on purpose: not a single booking (counted in one
/// notice), or a booking of a type that is not income.
inline bool isIgnoredCashTransaction(const pugi::xml_node& aRow) {
    if (!hasAttribute(aRow, "levelOfDetail"))
    {
        return false;
    }

    const auto level = valueOf(aRow, "levelOfDetail");

    // An empty level is rejected.
    if (level.empty())
    {
        return false;
    }

    if (level != "DETAIL")
    {
        return true;
    }

    if (!hasAttribute(aRow, "transactionID") || !hasAttribute(aRow, "type") ||
        !hasAttribute(aRow, "assetCategory"))
    {
        return false;
    }

    const auto type = valueOf(aRow, "type");

    return std::find(kIgnoredCashTypes.begin(), kIgnoredCashTypes.end(), type) !=
           kIgnoredCashTypes.end();
}

/// A `CorporateAction` the parser leaves out on purpose: a dividend or payment row, because the
/// cash row is the payment.
inline bool isIgnoredCorporateAction(const pugi::xml_node& aRow) {
    if (!hasAttribute(aRow, "transactionID") || !hasAttribute(aRow, "type") ||
        !hasAttribute(aRow, "assetCategory"))
    {
        return false;
    }

    const auto type = valueOf(aRow, "type");

    return std::find(kIgnoredActionTypes.begin(), kIgnoredActionTypes.end(), type) !=
           kIgnoredActionTypes.end();
}

template <typename Predicate>
void collectSection(const pugi::xml_node& aSection,
                    const char* aRowName,
                    const std::vector<std::size_t>& aLineStarts,
                    const Predicate& aIsIgnored,
                    std::vector<std::size_t>& aRows) {
    for (const pugi::xml_node row : aSection.children(aRowName))
    {
        if (!aIsIgnored(row))
        {
            aRows.push_back(detail::lineOf(row, aLineStarts));
        }
    }
}

} // namespace detail

/// The lines of the rows that must end as an event or an error: every `Trade`, `CashTransaction`
/// and `CorporateAction` that the parser reads, except the rows it leaves out on purpose. Gives an
/// empty list for a text the parser rejects as a whole (not XML, a document type, no
/// `FlexQueryResponse` root). Two rows on one line count once, so the check is weaker there.
inline std::vector<std::size_t> rowsToCheck(std::string_view aText) {
    std::vector<std::size_t> rows;

    try
    {
        pugi::xml_document document;
        const auto loaded = document.load_buffer(aText.data(),
                                                 aText.size(),
                                                 pugi::parse_default | pugi::parse_doctype,
                                                 pugi::encoding_utf8);

        // pugixml's own search: Clang with warnings as errors rejects std::any_of over the nodes.
        const bool hasDoctype = loaded.status == pugi::status_bad_doctype ||
                                !document
                                     .find_child([](const pugi::xml_node& aNode) {
                                         return aNode.type() == pugi::node_doctype;
                                     })
                                     .empty();
        const auto root = document.child("FlexQueryResponse");

        if (hasDoctype || !loaded || root.empty())
        {
            return rows;
        }

        const auto lineStarts = detail::lineStartsOf(aText);

        for (const pugi::xml_node statement :
             root.child("FlexStatements").children("FlexStatement"))
        {
            for (const pugi::xml_node section : statement.children())
            {
                const std::string_view name = section.name();

                if (name == "Trades")
                {
                    detail::collectSection(section,
                                           "Trade",
                                           lineStarts,
                                           detail::isIgnoredTrade,
                                           rows);
                }
                else if (name == "CorporateActions")
                {
                    detail::collectSection(section,
                                           "CorporateAction",
                                           lineStarts,
                                           detail::isIgnoredCorporateAction,
                                           rows);
                }
                else if (name == "CashTransactions")
                {
                    detail::collectSection(section,
                                           "CashTransaction",
                                           lineStarts,
                                           detail::isIgnoredCashTransaction,
                                           rows);
                }
            }
        }
    } catch (...)
    { rows.clear(); }

    return rows;
}

} // namespace taxbroker::test::ibkr
