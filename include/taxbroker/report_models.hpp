#pragma once

#include "taxbroker/statement_merger.hpp"
#include "taxbroker/types.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace taxbroker {

enum class FilingType {
    Original,
    SelfReport,
};

/// Taxpayer details shared by the headers of all four forms. Only the tax number and the
/// residence are required.
struct TaxpayerMetadata {
    /// Eight digits. Checking the value belongs to the code that reads the user's input.
    std::string mTaxNumber;
    std::optional<std::string> mName;
    std::optional<std::string> mAddress;
    std::optional<std::string> mCity;
    std::optional<std::string> mPostCode;
    std::optional<std::string> mPostName;
    std::optional<Date> mBirthDate;
    bool mIsResident{true};
    std::optional<std::string> mEmail;
    std::optional<std::string> mPhoneNumber;

    bool operator==(const TaxpayerMetadata&) const = default;
};

/// Metadata of one report run. It is held once and handed to each form writer next to the model
/// of that form; no form model repeats it.
struct ReportMetadata {
    TaxpayerMetadata mTaxpayer;
    int mReportingYear{};
    FilingType mFilingType{FilingType::Original};

    bool operator==(const ReportMetadata&) const = default;
};

/// Doh-KDVP F2, the way of purchase. `B` is the way for every stock purchase.
enum class KdvpPurchaseMethod {
    B,
};

/// Doh-KDVP F10 of a sale row. There is no "unresolved" value on purpose: a sale whose loss check
/// is not resolved gets no row, so an open question is never written as a decision.
enum class KdvpLossRule {
    /// The sale has no loss, or the loss may reduce taxable gains.
    NotBlocked,
    /// The 30-day loss rule blocks the loss of this row's units.
    Blocked,
};

/// Lists only the units that sales of the year use within the holding period. A lot a split
/// changed lists the units after the split and the exact basis over them (F4, rounded to 8
/// decimals). A purchase sold before and after a split has one row for each run of the same state.
struct KdvpPurchaseRow {
    StatementEventReference mPurchase;
    Date mDate;
    KdvpPurchaseMethod mMethod{KdvpPurchaseMethod::B};
    Units mUnits{};
    /// In EUR.
    UnitPrice mUnitPrice{};

    bool operator==(const KdvpPurchaseRow&) const = default;
};

/// A sale whose loss the 30-day rule blocks only in part has one row for each run of units with
/// the same F10; their units add up to the listed units of the sale.
struct KdvpSaleRow {
    StatementEventReference mSale;
    Date mDate;
    Units mUnits{};
    UnitPrice mUnitPrice{};
    KdvpLossRule mLossRule{KdvpLossRule::NotBlocked};

    bool operator==(const KdvpSaleRow&) const = default;
};

/// Purchase or a sale, followed by the stock.
struct KdvpRow {
    std::variant<KdvpPurchaseRow, KdvpSaleRow> mEntry;
    /// Listed purchase units minus the listed sale units up to and including this row.
    Units mStock{};

    bool operator==(const KdvpRow&) const = default;
};

/// One *popisni list* for one ISIN that has a sale in the selected year. The rows follow the
/// ledger order. It holds no FIFO matches.
struct KdvpInventoryList {
    Isin mIsin;
    /// Empty when no name was supplied.
    std::string mName;
    /// IsFond. Always false for now; the class check belongs to the classification gate.
    bool mIsFund{};
    std::vector<KdvpRow> mRows;

    bool operator==(const KdvpInventoryList&) const = default;
};

/// The Doh-KDVP model: one list for each ISIN with a sale in the selected year, by ISIN.
struct CapitalGainsReportKDVP {
    std::vector<KdvpInventoryList> mLists;

    bool operator==(const CapitalGainsReportKDVP&) const = default;
};

} // namespace taxbroker
