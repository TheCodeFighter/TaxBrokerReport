#pragma once

#include "taxbroker/event_metadata.hpp"
#include "taxbroker/reference_catalogue.hpp"
#include "taxbroker/statement_merger.hpp"
#include "taxbroker/types.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace taxbroker {

/// The payer facts of an income row. A fact the app could not find is empty: the writer leaves
/// its element out, the builder warns, and the user completes it in eDavki. A payer with several
/// reference rows gets the facts of the row the user picked, or none.
struct IncomePayer {
    std::optional<std::string> mName;                 ///< Legal name.
    std::optional<std::string> mAddress;              ///< One line.
    std::optional<std::string> mCountry;              ///< Residence country, 2 uppercase letters.
    std::optional<std::string> mTaxNumber;            ///< Slovenian tax number.
    std::optional<std::string> mIdentificationNumber; ///< A foreign payer's tax or other number.
    std::optional<CatalogueSource> mCatalogueRow;     ///< The reference row; empty: not found.

    bool operator==(const IncomePayer&) const = default;
};

/// Foreign tax of one payment. A blank tax in the export is a tax of 0. A payment of a Slovenian
/// payer is never a row: the company deducts the tax itself and the payment is an excluded item.
struct ForeignWithholding {
    Money mForeignTax{};   ///< EUR at the ECB rate of the receipt date; 0 is no tax paid.
    bool mBlankInExport{}; ///< The export had no tax, so mForeignTax is 0.

    bool operator==(const ForeignWithholding&) const = default;
};

/// A treaty claim of a row with foreign tax: the reference text of its country or one the user
/// typed. The text is written as the ReliefStatement. A row without one makes no claim.
struct TreatyClaim {
    std::string mStatement;                       ///< The treaty article text, written to the XML.
    std::optional<CatalogueSource> mCatalogueRow; ///< The relief-statement row; empty: typed.

    bool operator==(const TreatyClaim&) const = default;
};

/// What the payment is. The Doh-Div writer maps it to the form's `Type` code.
enum class DividendKind {
    Ordinary, ///< An ordinary dividend.
};

/// One dividend payment. Amounts are EUR at 4 decimals; the writer rounds to 2.
struct DivDividendRow {
    StatementEventReference mPayment;          ///< The source event.
    Isin mIsin;                                ///< Not written; for the summary and the warnings.
    Date mDate;                                ///< Receipt date, inside the reporting year.
    IncomePayer mPayer;                        ///< May be empty.
    DividendKind mKind{};                      ///< Always set by the creator.
    Money mGrossIncome{};                      ///< EUR, before any tax.
    ForeignWithholding mWithholding;           ///< Always set by the creator.
    std::optional<std::string> mSourceCountry; ///< 2 uppercase letters; empty when not known.
    std::optional<TreatyClaim> mTreatyClaim;   ///< Empty: no claim.

    bool operator==(const DivDividendRow&) const = default;
};

/// The Doh-Div model: rows in date order, then ledger order.
struct DividendReportDiv {
    std::vector<DivDividendRow> mRows;

    bool operator==(const DividendReportDiv&) const = default;
};

/// What the interest is. The Doh-DHO writer maps it to the form's `InterestType` code.
enum class DepositInterestKind {
    BankDeposit, ///< Qualifying bank-deposit interest.
};

/// The imported interest of one payer, interest kind and source country. The user adds the
/// interest of other banks in eDavki; the app takes no amount for it.
struct DhoInterestRow {
    std::vector<StatementEventReference> mPayments; ///< The imported payments of the group.
    IncomePayer mPayer;                             ///< May be empty.
    DepositInterestKind mKind{};                    ///< Always set by the creator.
    Money mInterest{};                              ///< EUR, exact sum of the payments.
    Money mForeignTax{};                            ///< EUR, exact sum; a blank tax counts 0.
    std::optional<std::string> mSourceCountry;      ///< 2 uppercase letters; empty when not known.

    bool operator==(const DhoInterestRow&) const = default;
};

/// The Doh-DHO model.
struct DepositInterestReportDHO {
    std::vector<DhoInterestRow> mRows;

    bool operator==(const DepositInterestReportDHO&) const = default;
};

/// Doh-Obr subtype: interest on the cash a broker holds. The writer maps it to its type code.
struct BrokerCashInterest {
    bool operator==(const BrokerCashInterest&) const = default;
};

/// Doh-Obr subtype: an ordinary bond coupon. The ISIN is kept for the Doh-Obr ISIN exclusion.
struct BondCoupon {
    Isin mIsin;

    bool operator==(const BondCoupon&) const = default;
};

/// One interest payment of the shared Doh-Obr form.
struct ObrInterestRow {
    StatementEventReference mPayment;                   ///< The source event.
    std::variant<BrokerCashInterest, BondCoupon> mKind; ///< Always set by the creator.
    Date mDate;                                         ///< Receipt date, inside the year.
    IncomePayer mPayer;                                 ///< May be empty.
    Money mGrossInterest{};                             ///< EUR, above zero.
    ForeignWithholding mWithholding;                    ///< Always set by the creator.
    std::optional<std::string> mSourceCountry;          ///< 2 letters; empty when not known.
    std::optional<TreatyClaim> mTreatyClaim;            ///< Not written until the claim rule.

    bool operator==(const ObrInterestRow&) const = default;
};

/// The Doh-Obr model: broker cash interest and bond coupons in one report.
struct OtherInterestReportObr {
    std::vector<ObrInterestRow> mRows;

    bool operator==(const OtherInterestReportObr&) const = default;
};

} // namespace taxbroker
