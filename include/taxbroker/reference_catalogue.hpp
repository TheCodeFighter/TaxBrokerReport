#pragma once

#include "taxbroker/event_metadata.hpp"
#include "taxbroker/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace taxbroker {

/// Where a reference row comes from. `mFile` is a file name without a path.
struct CatalogueSource {
    std::string mSource;         ///< "ib-edavki" or "manual".
    std::string mSourceRevision; ///< The upstream commit; empty for a "manual" row.
    std::string mFile;           ///< For example "payers.csv".
    std::size_t mRowNumber = 0;  ///< Logical row of the file, header = 1.
};

/// A payer of dividends. At least one of `mIsin` and `mIbkrConid` is set.
struct PayerEntry {
    std::optional<Isin> mIsin;                  ///< Checked: format and check digit.
    std::optional<std::string> mIbkrConid;      ///< Interactive Brokers contract number, digits.
    std::string mName;                          ///< The legal name.
    std::optional<std::string> mTaxNumber;      ///< None when the field is empty.
    std::string mAddress;                       ///< One line, as the source gives it.
    std::string mCountry;                       ///< 2 uppercase letters.
    std::optional<std::string> mInstrumentKind; ///< "share", "etf", "fund" or "bond".
    std::optional<Date> mVerifiedOn;            ///< Shown to the user, never compared with a date.
    CatalogueSource mSource;                    ///< Provenance of the row.
};

/// The treaty text of one country for the relief statement field of Doh-Div.
struct ReliefStatementEntry {
    std::string mCountry;    ///< 2 uppercase letters.
    std::string mStatement;  ///< Slovenian citation of the treaty article.
    CatalogueSource mSource; ///< Provenance of the row.
};

/// A broker company that pays interest, for Doh-Obr.
struct BrokerPayerEntry {
    std::string mBroker;                   ///< For example "IBKR".
    std::string mCode;                     ///< The broker's code of the company.
    std::string mName;                     ///< The legal name.
    std::optional<std::string> mTaxNumber; ///< None when the field is empty.
    std::string mAddress;                  ///< One line, as the source gives it.
    std::string mCountry;                  ///< 2 uppercase letters.
    CatalogueSource mSource;               ///< Provenance of the row.
};

/// What a payer lookup found.
enum class PayerLookupStatus {
    Found,    ///< One entry, or several with the same facts. A suggestion the user confirms.
    NotFound, ///< No entry has the identifier.
    Conflict, ///< Entries with different facts for the identifiers.
};

/// The result of `ReferenceCatalogue::findPayer`.
struct PayerLookup {
    PayerLookupStatus mStatus = PayerLookupStatus::NotFound; ///< What was found.
    std::vector<const PayerEntry*> mEntries; ///< Found: the first entry; Conflict: all of them.
};

/// The identifiers of a payer lookup. A ticker, a name or a country never matches.
struct PayerQuery {
    std::optional<Isin> mIsin;             ///< Matches the exact ISIN of an entry.
    std::optional<std::string> mIbkrConid; ///< Only for an Interactive Brokers event.
};

/// The payer reference data: payers, relief statements and broker payers.
class ReferenceCatalogue {
  public:
    ReferenceCatalogue(std::vector<PayerEntry> aPayers,
                       std::vector<ReliefStatementEntry> aReliefStatements,
                       std::vector<BrokerPayerEntry> aBrokerPayers);

    /// Finds the entries with the exact ISIN or the exact `conid` of the query, in file order. An
    /// entry found by both counts once. Several entries with the same facts (name, tax number,
    /// address, country, kind) are one Found; any difference is a Conflict.
    [[nodiscard]] PayerLookup findPayer(const PayerQuery& aQuery) const;

    /// Every relief statement of the country, in file order.
    [[nodiscard]] std::vector<const ReliefStatementEntry*>
    reliefStatements(std::string_view aCountry) const;

    /// The broker payer with this broker and code, or null.
    [[nodiscard]] const BrokerPayerEntry* brokerPayer(std::string_view aBroker,
                                                      std::string_view aCode) const;

    [[nodiscard]] const std::vector<PayerEntry>& payers() const;
    [[nodiscard]] const std::vector<ReliefStatementEntry>& allReliefStatements() const;
    [[nodiscard]] const std::vector<BrokerPayerEntry>& brokerPayers() const;

  private:
    std::vector<PayerEntry> mPayers;
    std::vector<ReliefStatementEntry> mReliefStatements;
    std::vector<BrokerPayerEntry> mBrokerPayers;
};

/// The text of `resources/reference/payers.csv`, embedded at build time.
[[nodiscard]] std::string_view embeddedPayersCsv();

/// The text of `resources/reference/relief_statements.csv`, embedded at build time.
[[nodiscard]] std::string_view embeddedReliefStatementsCsv();

/// The text of `resources/reference/broker_payers.csv`, embedded at build time.
[[nodiscard]] std::string_view embeddedBrokerPayersCsv();

/// Reads the three files. Throws std::logic_error with the file name and the logical row of the
/// first fault, never with a value of the file.
[[nodiscard]] ReferenceCatalogue parseReferenceCatalogue(std::string_view aPayersCsv,
                                                         std::string_view aReliefStatementsCsv,
                                                         std::string_view aBrokerPayersCsv);

/// The embedded files, parsed once.
[[nodiscard]] const ReferenceCatalogue& referenceCatalogue();

} // namespace taxbroker
