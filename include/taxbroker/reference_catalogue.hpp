#pragma once

#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/event_metadata.hpp"
#include "taxbroker/input_file.hpp"
#include "taxbroker/types.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace taxbroker {

/// Whether a reference file ships with the app or was supplied by the user.
enum class CatalogueOrigin {
    BuiltIn,
    Supplied,
};

/// Where a reference row comes from. `mFile` is a file name without a path.
struct CatalogueSource {
    std::string mSource;         ///< "ib-edavki" or "manual".
    std::string mSourceRevision; ///< The upstream commit; empty for a "manual" row.
    std::string mFile;           ///< For example "payers.csv".
    std::size_t mRowNumber = 0;  ///< Logical row of the file, header = 1.
    CatalogueOrigin mOrigin{};   ///< Where the file came from.

    bool operator==(const CatalogueSource&) const = default;
};

/// A payer of dividends, found by its ISIN.
struct PayerEntry {
    Isin mIsin;                                 ///< Checked: format and check digit.
    std::string mIsinSource;                    ///< From the web.
    std::string mName;                          ///< The legal name.
    std::optional<std::string> mTaxNumber;      ///< None when the field is empty.
    std::string mAddress;                       ///< One line, as the source gives it.
    std::string mCountry;                       ///< 2 uppercase letters.
    std::optional<std::string> mInstrumentKind; ///< "share", "etf", "fund" or "bond".
    std::optional<Date> mVerifiedOn;            ///< Shown to the user, never compared with a date.
    CatalogueSource mSource;                    ///< Provenance of the row.
};

/// The kind of income a treaty text is for. The articles can differ per kind, so a text is never
/// used for another kind.
enum class TreatyIncome {
    Dividend,     ///< A dividend, Doh-Div.
    CashInterest, ///< Interest on the cash a broker holds, Doh-Obr.
    BondCoupon,   ///< A bond coupon, Doh-Obr.
};

/// The treaty text of one country and one kind of income, for the relief statement field of
/// Doh-Div and Doh-Obr.
struct ReliefStatementEntry {
    std::string mCountry;    ///< 2 uppercase letters.
    TreatyIncome mIncome{};  ///< The kind of income the text is for; always set by the creator.
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

/// The payers of a file, with an index from the ISIN to the entries that have it. The positions
/// in the index are in file order. Made once; lists are shared read-only.
struct PayerList {
    std::vector<PayerEntry> mEntries;                                     ///< In file order.
    std::map<std::string, std::vector<std::size_t>, std::less<>> mByIsin; ///< ISIN to positions.

    explicit PayerList(std::vector<PayerEntry> aEntries);
};

/// The broker payers of a file, with an index from the broker and the code to the first entry.
struct BrokerPayerList {
    std::vector<BrokerPayerEntry> mEntries;
    std::map<std::pair<std::string, std::string>, std::size_t, std::less<>> mByBrokerAndCode;

    explicit BrokerPayerList(std::vector<BrokerPayerEntry> aEntries);
};

/// What a payer lookup found.
enum class PayerLookupStatus {
    Found,    ///< One entry, or several with the same facts. Used without a question.
    NotFound, ///< No entry has the identifier.
    Conflict, ///< Entries with different facts for the identifiers.
};

/// The result of `ReferenceCatalogue::findPayer`.
struct PayerLookup {
    PayerLookupStatus mStatus = PayerLookupStatus::NotFound; ///< What was found.
    std::vector<const PayerEntry*> mEntries; ///< Found: the first entry; Conflict: all of them.
};

/// The payer reference data: payers, relief statements and broker payers. The three lists are
/// shared and read-only, so a copy of the catalogue copies no entry and a built-in list is shared
/// by every catalogue that uses it.
class ReferenceCatalogue {
  public:
    ReferenceCatalogue(std::vector<PayerEntry> aPayers,
                       std::vector<ReliefStatementEntry> aReliefStatements,
                       std::vector<BrokerPayerEntry> aBrokerPayers);

    /// Takes shared lists; none may be null.
    ReferenceCatalogue(std::shared_ptr<const PayerList> aPayers,
                       std::shared_ptr<const std::vector<ReliefStatementEntry>> aReliefStatements,
                       std::shared_ptr<const BrokerPayerList> aBrokerPayers);

    /// Finds the entries with the exact ISIN, in file order. A ticker, a name or a country never
    /// matches. Several entries with the same facts (name, tax number, address, country, kind)
    /// are one Found; any difference is a Conflict.
    [[nodiscard]] PayerLookup findPayer(std::string_view aIsin) const;

    /// Every relief statement of the country for the kind of income, in file order.
    [[nodiscard]] std::vector<const ReliefStatementEntry*>
    reliefStatements(std::string_view aCountry, TreatyIncome aIncome) const;

    /// The broker payer with this broker and code, or null.
    [[nodiscard]] const BrokerPayerEntry* brokerPayer(std::string_view aBroker,
                                                      std::string_view aCode) const;

    /// The broker payers of a broker, in file order; none for an unknown broker.
    [[nodiscard]] std::vector<const BrokerPayerEntry*> brokerPayersOf(Broker aBroker) const;

    [[nodiscard]] const std::vector<PayerEntry>& payers() const;
    [[nodiscard]] const std::vector<ReliefStatementEntry>& allReliefStatements() const;
    [[nodiscard]] const std::vector<BrokerPayerEntry>& brokerPayers() const;

  private:
    std::shared_ptr<const PayerList> mPayers;
    std::shared_ptr<const std::vector<ReliefStatementEntry>> mReliefStatements;
    std::shared_ptr<const BrokerPayerList> mBrokerPayers;
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

/// The embedded files, parsed once per process.
[[nodiscard]] const ReferenceCatalogue& referenceCatalogue();

/// The first fault of a reference file, with no value of the file in it.
struct ReferenceFileFault {
    std::string mFile;
    std::optional<std::size_t> mRow; ///< Logical row, header = 1; none for the whole file.
    DiagnosticMessage mMessage;
};

/// The result of `loadReferenceData`. Either the catalogue or the fault is set.
struct ReferenceDataLoad {
    std::optional<ReferenceCatalogue> mCatalogue; ///< Set when every file is valid.
    /// Order: payers, relief statements, broker payers.
    std::array<CatalogueOrigin, 3> mOrigins{
        CatalogueOrigin::BuiltIn, CatalogueOrigin::BuiltIn, CatalogueOrigin::BuiltIn};
    std::optional<ReferenceFileFault> mFault; ///< Set when a name or a file is not valid.
};

/**
 * The reference data of a run. Each file, named `payers.csv`, `relief_statements.csv` or
 * `broker_payers.csv`, replaces the built-in file of that name whole; the other names keep the
 * built-in file. An empty list gives the built-in data. Another name, or the same name twice, is
 * a fault before any file is read. A supplied file goes through the checks of the built-in files.
 */
[[nodiscard]] ReferenceDataLoad loadReferenceData(const std::vector<std::filesystem::path>& aFiles,
                                                  std::size_t aMaxFileBytes = kMaxInputFileBytes);

/// The same for files in memory; the name of each is the display file name (a folder part is
/// dropped). A file over the size limit, or one that runs out of memory, is a fault with its own
/// message. The app always uses the default limit; a smaller one lets a test use a small file.
[[nodiscard]] ReferenceDataLoad loadReferenceData(std::span<const InputFile> aFiles,
                                                  std::size_t aMaxFileBytes = kMaxInputFileBytes);

} // namespace taxbroker
