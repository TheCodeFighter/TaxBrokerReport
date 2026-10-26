#pragma once

#include <boost/multiprecision/cpp_int.hpp>
#include <pugixml.hpp>

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace taxbroker::test {

/// The result of a FIFO over the rows of one Doh-KDVP list, read back from the written XML. It
/// uses no production code, so the tests can compare it with the literal values in tax_rules.md.
struct KdvpListFifo {
    std::string mIsin;
    /// F8 of each row as written, without trailing zeros ("15", "0").
    std::vector<std::string> mWrittenF8;
    /// F8 of each row as this FIFO counts it: the listed purchase units minus the listed sale
    /// units so far.
    std::vector<std::string> mCountedF8;
    /// The gain of each sale row in EUR: F7 x F9 minus F4 of each listed purchase unit it takes.
    std::vector<std::string> mSaleGains;
    /// Rows this FIFO could not use. Empty when every row was read and every sale found enough
    /// listed purchase units.
    std::vector<std::string> mProblems;
};

namespace kdvp_row_fifo {

using Number = boost::multiprecision::cpp_int;

/// Digits after the decimal point of a quantity or a unit value (the F4 and F9 limit).
inline constexpr std::size_t kScale = 8;

/// 10 to the power of aExponent.
inline Number powerOfTen(std::size_t aExponent) {
    Number result = 1;

    for (std::size_t i = 0; i < aExponent; ++i)
    {
        result *= 10;
    }

    return result;
}

/// Plain decimal text ("10", "-3.5", "0.12345678") as a whole number at kScale decimals.
/// Returns false for any other text, or for more than kScale decimals.
inline bool readDecimal(std::string_view aText, Number& aValue) {
    const bool negative = !aText.empty() && aText.front() == '-';

    if (negative)
    {
        aText.remove_prefix(1);
    }

    const auto point = aText.find('.');
    const std::string_view whole = aText.substr(0, point);
    const std::string_view fraction =
        point == std::string_view::npos ? std::string_view{} : aText.substr(point + 1);

    if (whole.empty() || fraction.size() > kScale ||
        (point != std::string_view::npos && fraction.empty()))
    {
        return false;
    }

    Number value = 0;

    for (const std::string_view digits : {whole, fraction})
    {
        for (const char digit : digits)
        {
            if (digit < '0' || digit > '9')
            {
                return false;
            }

            value = value * 10 + (digit - '0');
        }
    }

    value *= powerOfTen(kScale - fraction.size());
    aValue = negative ? Number{-value} : value;

    return true;
}

/// A whole number at aScale decimals as text without trailing zeros ("250", "-3.5", "0").
inline std::string writeDecimal(const Number& aValue, std::size_t aScale) {
    const Number factor = powerOfTen(aScale);
    const Number magnitude = aValue < 0 ? Number{-aValue} : aValue;
    std::string text = Number{magnitude / factor}.str();
    std::string fraction = Number{magnitude % factor}.str();

    fraction.insert(0, aScale - fraction.size(), '0');

    while (!fraction.empty() && fraction.back() == '0')
    {
        fraction.pop_back();
    }

    if (!fraction.empty())
    {
        text += "." + fraction;
    }

    return aValue < 0 ? "-" + text : text;
}

/// The units of one listed purchase row that no sale row has taken yet.
struct Lot {
    Number mUnits;
    Number mUnitValue;
};

} // namespace kdvp_row_fifo

/// Reads every `KDVPItem` of a Doh-KDVP document in order and runs a FIFO over its rows: a
/// purchase row adds F3 units at F4, and a sale row takes F7 units from the oldest listed
/// purchase units.
inline std::vector<KdvpListFifo> fifoOverKdvpRows(const std::string& aXml) {
    using kdvp_row_fifo::kScale;
    using kdvp_row_fifo::Lot;
    using kdvp_row_fifo::Number;
    using kdvp_row_fifo::readDecimal;
    using kdvp_row_fifo::writeDecimal;

    pugi::xml_document document;
    std::vector<KdvpListFifo> lists;

    if (!document.load_string(aXml.c_str()))
    {
        lists.push_back({.mProblems = {"The XML could not be read."}});

        return lists;
    }

    const auto report = document.child("Envelope").child("body").child("Doh_KDVP");

    for (const auto item : report.children("KDVPItem"))
    {
        const auto securities = item.child("Securities");
        KdvpListFifo list{.mIsin = securities.child_value("ISIN")};
        std::deque<Lot> lots;
        Number stock = 0;

        for (const auto row : securities.children("Row"))
        {
            const std::string rowId = row.child_value("ID");
            const auto readField = [&list, &rowId](const pugi::xml_node aParent,
                                                   const char* aName) {
                Number value = 0;

                if (!readDecimal(aParent.child_value(aName), value))
                {
                    list.mProblems.push_back("Row " + rowId + ": " + aName +
                                             " is not a plain decimal number.");
                }

                return value;
            };
            const auto purchase = row.child("Purchase");
            const auto sale = row.child("Sale");

            if (purchase)
            {
                const Number units = readField(purchase, "F3");
                const Number unitValue = readField(purchase, "F4");

                lots.push_back({.mUnits = units, .mUnitValue = unitValue});
                stock += units;
            }
            else if (sale)
            {
                const Number units = readField(sale, "F7");
                const Number unitValue = readField(sale, "F9");
                Number unitsLeft = units;
                Number purchaseValue = 0;

                while (unitsLeft > 0 && !lots.empty())
                {
                    Lot& oldest = lots.front();
                    const Number taken = unitsLeft < oldest.mUnits ? unitsLeft : oldest.mUnits;

                    purchaseValue += taken * oldest.mUnitValue;
                    oldest.mUnits -= taken;
                    unitsLeft -= taken;

                    if (oldest.mUnits == 0)
                    {
                        lots.pop_front();
                    }
                }

                if (unitsLeft > 0)
                {
                    list.mProblems.push_back("Row " + rowId +
                                             ": the sale needs more units than the listed "
                                             "purchase rows have.");
                }

                stock -= units;

                // Units and unit values have kScale decimals each, so their product has twice
                // as many.
                list.mSaleGains.push_back(
                    writeDecimal(units * unitValue - purchaseValue, 2 * kScale));
            }
            else
            {
                list.mProblems.push_back("Row " + rowId + " has neither Purchase nor Sale.");
            }

            list.mWrittenF8.push_back(writeDecimal(readField(row, "F8"), kScale));
            list.mCountedF8.push_back(writeDecimal(stock, kScale));
        }

        lists.push_back(std::move(list));
    }

    return lists;
}

} // namespace taxbroker::test
