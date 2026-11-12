#include "taxbroker/reference_catalogue.hpp"

#include "utils/csv_text_reader.hpp"
#include "utils/date_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace taxbroker {

namespace {

constexpr std::string_view kPayersFile = "payers.csv";
constexpr std::string_view kReliefStatementsFile = "relief_statements.csv";
constexpr std::string_view kBrokerPayersFile = "broker_payers.csv";

constexpr std::array<std::string_view, 10> kPayerColumns{"isin",
                                                         "name",
                                                         "tax_number",
                                                         "address",
                                                         "country",
                                                         "instrument_kind",
                                                         "verified_on",
                                                         "isin_source",
                                                         "source",
                                                         "source_revision"};
constexpr std::array<std::string_view, 5> kReliefStatementColumns{
    "country", "income", "statement", "source", "source_revision"};
constexpr std::array<std::string_view, 8> kBrokerPayerColumns{
    "broker", "code", "name", "tax_number", "address", "country", "source", "source_revision"};

/// A fault of a reference file. `what()` is the developer text (file, row, what is wrong); the
/// fault carries the same fact as a message key for the user.
class ReferenceFileError : public std::logic_error {
  public:
    ReferenceFileError(const std::string& aText, ReferenceFileFault aFault)
        : std::logic_error(aText), mFault(std::move(aFault)) {}

    [[nodiscard]] const ReferenceFileFault& fault() const {
        return mFault;
    }

  private:
    ReferenceFileFault mFault;
};

/// Throws the fault of a row: the file, the logical row and what is wrong, never a value.
[[noreturn]] void failRow(std::string_view aFile,
                          std::size_t aRow,
                          std::string_view aWhat,
                          MessageKey aKey,
                          std::vector<MessageParam> aParams = {}) {
    throw ReferenceFileError{
        std::string{aFile} + " row " + std::to_string(aRow) + ": " + std::string{aWhat},
        ReferenceFileFault{.mFile = std::string{aFile},
                           .mRow = aRow,
                           .mMessage = makeDiagnosticMessage(aKey, std::move(aParams))}};
}

/// Throws the fault of a whole file.
[[noreturn]] void failFile(std::string_view aFile,
                           std::string_view aWhat,
                           MessageKey aKey,
                           std::vector<MessageParam> aParams = {}) {
    throw ReferenceFileError{
        std::string{aFile} + ": " + std::string{aWhat},
        ReferenceFileFault{.mFile = std::string{aFile},
                           .mRow = std::nullopt,
                           .mMessage = makeDiagnosticMessage(aKey, std::move(aParams))}};
}

std::string joinColumns(const std::vector<std::string>& aColumns) {
    std::string joined;

    for (const auto& column : aColumns)
    {
        joined += joined.empty() ? column : ", " + column;
    }

    return joined;
}

/// Reads one file with the shared reader and throws on its first fault.
CsvTable readFile(std::string_view aFile,
                  std::string_view aText,
                  std::span<const std::string_view> aRequiredColumns) {
    const CsvTable table = readCsvText(aText, ',', aRequiredColumns);

    if (!table.mFault)
    {
        return table;
    }

    const CsvFault& fault = *table.mFault;

    switch (fault.mKind)
    {
    case CsvFaultKind::UnclosedQuote:
        failFile(aFile, "a quote is not closed", MessageKey::ReferenceFileUnclosedQuote);
    case CsvFaultKind::MissingColumns:
        failFile(aFile,
                 "the header lacks the columns " + joinColumns(fault.mColumns),
                 MessageKey::ReferenceFileMissingColumns,
                 {numberParam("count", fault.mColumns.size()),
                  textParam("columns", joinColumns(fault.mColumns))});
    case CsvFaultKind::RepeatedColumn:
        failFile(aFile,
                 "the header has the column " + joinColumns(fault.mColumns) + " twice",
                 MessageKey::ReferenceFileRepeatedColumn,
                 {textParam("column", joinColumns(fault.mColumns))});
    case CsvFaultKind::LineBreakInField:
        failRow(aFile,
                fault.mRowNumber.value_or(0),
                "a field holds a line break",
                MessageKey::ReferenceFileLineBreak);
    case CsvFaultKind::FieldCountMismatch:
        failRow(aFile,
                fault.mRowNumber.value_or(0),
                "the row has " + std::to_string(fault.mFoundFields) + " fields, the header " +
                    std::to_string(fault.mExpectedFields),
                MessageKey::ReferenceFileFieldCount,
                {numberParam("expected", fault.mExpectedFields),
                 numberParam("found", fault.mFoundFields)});
    case CsvFaultKind::Unreadable:
        failFile(aFile, "the file cannot be read", MessageKey::ReferenceFileUnreadable);
    }

    failFile(aFile, "unknown fault", MessageKey::ReferenceFileUnreadable);
}

bool isUpperLetter(char aChar) {
    return aChar >= 'A' && aChar <= 'Z';
}

bool isDigit(char aChar) {
    return aChar >= '0' && aChar <= '9';
}

bool isCountry(std::string_view aValue) {
    return aValue.size() == 2 && isUpperLetter(aValue[0]) && isUpperLetter(aValue[1]);
}

/// ISO 6166: 2 letters, 9 letters or digits and a check digit. Letters count as 10 to 35, and
/// the check digit makes the Luhn sum of the expanded digits a multiple of 10.
bool isValidIsin(std::string_view aValue) {
    if (aValue.size() != 12 || !isUpperLetter(aValue[0]) || !isUpperLetter(aValue[1]) ||
        !isDigit(aValue[11]))
    {
        return false;
    }

    std::string digits;

    for (const char character : aValue)
    {
        if (isDigit(character))
        {
            digits += character;
        }
        else if (isUpperLetter(character))
        {
            digits += std::to_string(10 + (character - 'A'));
        }
        else
        {
            return false;
        }
    }

    int sum = 0;
    bool doubled = false;

    for (auto digit = digits.rbegin(); digit != digits.rend(); ++digit)
    {
        int value = *digit - '0';

        if (doubled)
        {
            value *= 2;
            value = value > 9 ? value - 9 : value;
        }

        sum += value;
        doubled = !doubled;
    }

    return sum % 10 == 0;
}

std::optional<std::string> optionalText(const std::string& aValue) {
    if (aValue.empty())
    {
        return std::nullopt;
    }

    return aValue;
}

/// The provenance columns `source` and `source_revision` of a row, checked.
CatalogueSource readSource(std::string_view aFile,
                           std::size_t aRow,
                           const std::string& aSource,
                           const std::string& aSourceRevision,
                           CatalogueOrigin aOrigin) {
    if (aSource != "ib-edavki" && aSource != "manual")
    {
        failRow(aFile,
                aRow,
                "the source is not ib-edavki or manual",
                MessageKey::ReferenceFileInvalidSource);
    }

    if (aSource == "ib-edavki" && aSourceRevision.empty())
    {
        failRow(aFile,
                aRow,
                "an ib-edavki row has no source revision",
                MessageKey::ReferenceFileMissingSourceRevision);
    }

    return CatalogueSource{.mSource = aSource,
                           .mSourceRevision = aSourceRevision,
                           .mFile = std::string{aFile},
                           .mRowNumber = aRow,
                           .mOrigin = aOrigin};
}

/// Fails when the value is empty; `aField` is the column name.
void requireText(std::string_view aFile,
                 std::size_t aRow,
                 const std::string& aValue,
                 std::string_view aField) {
    if (aValue.empty())
    {
        failRow(aFile,
                aRow,
                "the " + std::string{aField} + " is empty",
                MessageKey::ReferenceFileEmptyField,
                {textParam("field", std::string{aField})});
    }
}

void requireCountry(std::string_view aFile, std::size_t aRow, const std::string& aValue) {
    if (!isCountry(aValue))
    {
        failRow(aFile,
                aRow,
                "the country is not 2 uppercase letters",
                MessageKey::ReferenceFileInvalidCountry);
    }
}

std::vector<PayerEntry> readPayers(std::string_view aText, CatalogueOrigin aOrigin) {
    const CsvTable table = readFile(kPayersFile, aText, kPayerColumns);
    const std::size_t isinColumn = table.columnIndex("isin");
    const std::size_t nameColumn = table.columnIndex("name");
    const std::size_t taxNumberColumn = table.columnIndex("tax_number");
    const std::size_t addressColumn = table.columnIndex("address");
    const std::size_t countryColumn = table.columnIndex("country");
    const std::size_t kindColumn = table.columnIndex("instrument_kind");
    const std::size_t verifiedColumn = table.columnIndex("verified_on");
    const std::size_t isinSourceColumn = table.columnIndex("isin_source");
    const std::size_t sourceColumn = table.columnIndex("source");
    const std::size_t revisionColumn = table.columnIndex("source_revision");

    std::vector<PayerEntry> payers;

    for (const CsvRow& row : table.mRows)
    {
        const std::size_t number = row.mRowNumber;
        const auto& fields = row.mFields;

        requireText(kPayersFile, number, fields[isinColumn], "isin");

        if (!isValidIsin(fields[isinColumn]))
        {
            failRow(kPayersFile,
                    number,
                    "the ISIN is not valid",
                    MessageKey::ReferenceFileInvalidIsin);
        }

        const std::string& isinSource = fields[isinSourceColumn];

        requireText(kPayersFile, number, isinSource, "isin_source");

        if (isinSource != "ib-edavki" && isinSource != "wikidata" && isinSource != "issuer" &&
            isinSource != "exchange" && isinSource != "gleif")
        {
            failRow(kPayersFile,
                    number,
                    "the ISIN source is not ib-edavki, wikidata, issuer, exchange or gleif",
                    MessageKey::ReferenceFileInvalidIsinSource);
        }

        requireText(kPayersFile, number, fields[nameColumn], "name");
        requireText(kPayersFile, number, fields[addressColumn], "address");
        requireCountry(kPayersFile, number, fields[countryColumn]);

        const std::string& kind = fields[kindColumn];

        if (!kind.empty() && kind != "share" && kind != "etf" && kind != "fund" && kind != "bond")
        {
            failRow(kPayersFile,
                    number,
                    "the instrument kind is not share, etf, fund or bond",
                    MessageKey::ReferenceFileInvalidInstrumentKind);
        }

        std::optional<Date> verifiedOn;

        if (!fields[verifiedColumn].empty())
        {
            verifiedOn = parseCalendarDate(fields[verifiedColumn]);

            if (!verifiedOn)
            {
                failRow(kPayersFile,
                        number,
                        "verified_on is not a date as YYYY-MM-DD",
                        MessageKey::ReferenceFileInvalidVerifiedOn);
            }
        }

        payers.push_back(PayerEntry{
            .mIsin = fields[isinColumn],
            .mIsinSource = isinSource,
            .mName = fields[nameColumn],
            .mTaxNumber = optionalText(fields[taxNumberColumn]),
            .mAddress = fields[addressColumn],
            .mCountry = fields[countryColumn],
            .mInstrumentKind = optionalText(kind),
            .mVerifiedOn = verifiedOn,
            .mSource = readSource(kPayersFile,
                                  number,
                                  fields[sourceColumn],
                                  fields[revisionColumn],
                                  aOrigin),
        });
    }

    return payers;
}

/// The kind of income of the `income` column, or none for another text.
std::optional<TreatyIncome> treatyIncomeOf(std::string_view aText) {
    if (aText == "dividend")
    {
        return TreatyIncome::Dividend;
    }

    if (aText == "cash_interest")
    {
        return TreatyIncome::CashInterest;
    }

    if (aText == "bond_coupon")
    {
        return TreatyIncome::BondCoupon;
    }

    return std::nullopt;
}

std::vector<ReliefStatementEntry> readReliefStatements(std::string_view aText,
                                                       CatalogueOrigin aOrigin) {
    const CsvTable table = readFile(kReliefStatementsFile, aText, kReliefStatementColumns);
    const std::size_t countryColumn = table.columnIndex("country");
    const std::size_t incomeColumn = table.columnIndex("income");
    const std::size_t statementColumn = table.columnIndex("statement");
    const std::size_t sourceColumn = table.columnIndex("source");
    const std::size_t revisionColumn = table.columnIndex("source_revision");

    std::vector<ReliefStatementEntry> statements;

    for (const CsvRow& row : table.mRows)
    {
        const std::size_t number = row.mRowNumber;
        const auto& fields = row.mFields;

        requireCountry(kReliefStatementsFile, number, fields[countryColumn]);

        const auto income = treatyIncomeOf(fields[incomeColumn]);

        if (!income)
        {
            failRow(kReliefStatementsFile,
                    number,
                    "the income is not dividend, cash_interest or bond_coupon",
                    MessageKey::ReferenceFileInvalidTreatyIncome);
        }

        requireText(kReliefStatementsFile, number, fields[statementColumn], "statement");

        statements.push_back(ReliefStatementEntry{
            .mCountry = fields[countryColumn],
            .mIncome = *income,
            .mStatement = fields[statementColumn],
            .mSource = readSource(kReliefStatementsFile,
                                  number,
                                  fields[sourceColumn],
                                  fields[revisionColumn],
                                  aOrigin),
        });
    }

    return statements;
}

std::vector<BrokerPayerEntry> readBrokerPayers(std::string_view aText, CatalogueOrigin aOrigin) {
    const CsvTable table = readFile(kBrokerPayersFile, aText, kBrokerPayerColumns);
    const std::size_t brokerColumn = table.columnIndex("broker");
    const std::size_t codeColumn = table.columnIndex("code");
    const std::size_t nameColumn = table.columnIndex("name");
    const std::size_t taxNumberColumn = table.columnIndex("tax_number");
    const std::size_t addressColumn = table.columnIndex("address");
    const std::size_t countryColumn = table.columnIndex("country");
    const std::size_t sourceColumn = table.columnIndex("source");
    const std::size_t revisionColumn = table.columnIndex("source_revision");

    std::vector<BrokerPayerEntry> brokerPayers;

    for (const CsvRow& row : table.mRows)
    {
        const std::size_t number = row.mRowNumber;
        const auto& fields = row.mFields;

        requireText(kBrokerPayersFile, number, fields[brokerColumn], "broker");
        requireText(kBrokerPayersFile, number, fields[codeColumn], "code");
        requireText(kBrokerPayersFile, number, fields[nameColumn], "name");
        requireText(kBrokerPayersFile, number, fields[addressColumn], "address");
        requireCountry(kBrokerPayersFile, number, fields[countryColumn]);

        const bool repeated = std::any_of(brokerPayers.begin(),
                                          brokerPayers.end(),
                                          [&](const BrokerPayerEntry& aEntry) {
                                              return aEntry.mBroker == fields[brokerColumn] &&
                                                     aEntry.mCode == fields[codeColumn];
                                          });

        if (repeated)
        {
            failRow(kBrokerPayersFile,
                    number,
                    "the broker and code are already in the file",
                    MessageKey::ReferenceFileDuplicateBrokerCode);
        }

        brokerPayers.push_back(BrokerPayerEntry{
            .mBroker = fields[brokerColumn],
            .mCode = fields[codeColumn],
            .mName = fields[nameColumn],
            .mTaxNumber = optionalText(fields[taxNumberColumn]),
            .mAddress = fields[addressColumn],
            .mCountry = fields[countryColumn],
            .mSource = readSource(kBrokerPayersFile,
                                  number,
                                  fields[sourceColumn],
                                  fields[revisionColumn],
                                  aOrigin),
        });
    }

    return brokerPayers;
}

/// True when the two entries give the same facts about the payer (provenance is not a fact).
bool sameFacts(const PayerEntry& aLeft, const PayerEntry& aRight) {
    return aLeft.mName == aRight.mName && aLeft.mTaxNumber == aRight.mTaxNumber &&
           aLeft.mAddress == aRight.mAddress && aLeft.mCountry == aRight.mCountry &&
           aLeft.mInstrumentKind == aRight.mInstrumentKind;
}

} // namespace

PayerList::PayerList(std::vector<PayerEntry> aEntries) : mEntries(std::move(aEntries)) {
    for (std::size_t position = 0; position < mEntries.size(); ++position)
    {
        mByIsin[mEntries[position].mIsin].push_back(position);
    }
}

BrokerPayerList::BrokerPayerList(std::vector<BrokerPayerEntry> aEntries)
    : mEntries(std::move(aEntries)) {
    for (std::size_t position = 0; position < mEntries.size(); ++position)
    {
        // The first entry of a broker and code wins, as in a search from the start.
        mByBrokerAndCode.emplace(
            std::make_pair(mEntries[position].mBroker, mEntries[position].mCode),
            position);
    }
}

ReferenceCatalogue::ReferenceCatalogue(std::vector<PayerEntry> aPayers,
                                       std::vector<ReliefStatementEntry> aReliefStatements,
                                       std::vector<BrokerPayerEntry> aBrokerPayers)
    : mPayers(std::make_shared<const PayerList>(std::move(aPayers))),
      mReliefStatements(
          std::make_shared<const std::vector<ReliefStatementEntry>>(std::move(aReliefStatements))),
      mBrokerPayers(std::make_shared<const BrokerPayerList>(std::move(aBrokerPayers))) {}

ReferenceCatalogue::ReferenceCatalogue(
    std::shared_ptr<const PayerList> aPayers,
    std::shared_ptr<const std::vector<ReliefStatementEntry>> aReliefStatements,
    std::shared_ptr<const BrokerPayerList> aBrokerPayers)
    : mPayers(std::move(aPayers)), mReliefStatements(std::move(aReliefStatements)),
      mBrokerPayers(std::move(aBrokerPayers)) {
    if (!mPayers || !mReliefStatements || !mBrokerPayers)
    {
        throw std::logic_error{"A reference catalogue needs all three lists"};
    }
}

PayerLookup ReferenceCatalogue::findPayer(std::string_view aIsin) const {
    const auto found = mPayers->mByIsin.find(aIsin);

    if (found == mPayers->mByIsin.end())
    {
        return PayerLookup{};
    }

    // The index holds the positions in file order.
    std::vector<const PayerEntry*> matches;

    matches.reserve(found->second.size());
    std::transform(found->second.begin(),
                   found->second.end(),
                   std::back_inserter(matches),
                   [this](std::size_t aPosition) { return &mPayers->mEntries[aPosition]; });

    const bool allSame = std::all_of(matches.begin(), matches.end(), [&](const PayerEntry* aEntry) {
        return sameFacts(*matches.front(), *aEntry);
    });

    if (allSame)
    {
        return PayerLookup{.mStatus = PayerLookupStatus::Found, .mEntries = {matches.front()}};
    }

    return PayerLookup{.mStatus = PayerLookupStatus::Conflict, .mEntries = std::move(matches)};
}

std::vector<const ReliefStatementEntry*>
ReferenceCatalogue::reliefStatements(std::string_view aCountry, TreatyIncome aIncome) const {
    std::vector<const ReliefStatementEntry*> statements;

    for (const ReliefStatementEntry& entry : *mReliefStatements)
    {
        if (entry.mCountry == aCountry && entry.mIncome == aIncome)
        {
            statements.push_back(&entry);
        }
    }

    return statements;
}

const BrokerPayerEntry* ReferenceCatalogue::brokerPayer(std::string_view aBroker,
                                                        std::string_view aCode) const {
    const auto found = mBrokerPayers->mByBrokerAndCode.find(
        std::make_pair(std::string{aBroker}, std::string{aCode}));

    if (found == mBrokerPayers->mByBrokerAndCode.end())
    {
        return nullptr;
    }

    return &mBrokerPayers->mEntries[found->second];
}

namespace {

/// The `broker` text of `broker_payers.csv` for a broker; empty when the file has none for it.
std::string_view brokerPayersKey(Broker aBroker) {
    switch (aBroker)
    {
    case Broker::TradeRepublic:
        return "TR";
    case Broker::InteractiveBrokers:
        return "IBKR";
    case Broker::Unknown:
        break;
    }

    return {};
}

} // namespace

std::vector<const BrokerPayerEntry*> ReferenceCatalogue::brokerPayersOf(Broker aBroker) const {
    std::vector<const BrokerPayerEntry*> entries;
    const auto key = brokerPayersKey(aBroker);

    for (const auto& entry : mBrokerPayers->mEntries)
    {
        if (!key.empty() && entry.mBroker == key)
        {
            entries.push_back(&entry);
        }
    }

    return entries;
}

const std::vector<PayerEntry>& ReferenceCatalogue::payers() const {
    return mPayers->mEntries;
}

const std::vector<ReliefStatementEntry>& ReferenceCatalogue::allReliefStatements() const {
    return *mReliefStatements;
}

const std::vector<BrokerPayerEntry>& ReferenceCatalogue::brokerPayers() const {
    return mBrokerPayers->mEntries;
}

ReferenceCatalogue parseReferenceCatalogue(std::string_view aPayersCsv,
                                           std::string_view aReliefStatementsCsv,
                                           std::string_view aBrokerPayersCsv) {
    return ReferenceCatalogue{readPayers(aPayersCsv, CatalogueOrigin::BuiltIn),
                              readReliefStatements(aReliefStatementsCsv, CatalogueOrigin::BuiltIn),
                              readBrokerPayers(aBrokerPayersCsv, CatalogueOrigin::BuiltIn)};
}

namespace {

/// The three built-in lists, parsed once per process and shared by every catalogue that uses
/// them. A fault in an embedded file is a build error and throws on the first use.
struct BuiltInLists {
    std::shared_ptr<const PayerList> mPayers;
    std::shared_ptr<const std::vector<ReliefStatementEntry>> mReliefStatements;
    std::shared_ptr<const BrokerPayerList> mBrokerPayers;
};

const BuiltInLists& builtInLists() {
    static const BuiltInLists lists{
        std::make_shared<const PayerList>(
            readPayers(embeddedPayersCsv(), CatalogueOrigin::BuiltIn)),
        std::make_shared<const std::vector<ReliefStatementEntry>>(
            readReliefStatements(embeddedReliefStatementsCsv(), CatalogueOrigin::BuiltIn)),
        std::make_shared<const BrokerPayerList>(
            readBrokerPayers(embeddedBrokerPayersCsv(), CatalogueOrigin::BuiltIn)),
    };

    return lists;
}

} // namespace

const ReferenceCatalogue& referenceCatalogue() {
    static const ReferenceCatalogue catalogue{builtInLists().mPayers,
                                              builtInLists().mReliefStatements,
                                              builtInLists().mBrokerPayers};

    return catalogue;
}

namespace {

/// The text of one supplied file, or why there is none.
using ReferenceText = std::variant<std::string, InputFileFault>;

/// The reference data of a run. aNames are the file names of the supplied files and aTextOf(i)
/// gives the text of the i-th one. The names are checked before any text is taken.
template <typename TextOf>
ReferenceDataLoad loadReferenceFiles(const std::vector<std::string>& aNames,
                                     const TextOf& aTextOf,
                                     const std::size_t aMaxFileBytes) {
    constexpr std::array<std::string_view, 3> names{kPayersFile,
                                                    kReliefStatementsFile,
                                                    kBrokerPayersFile};

    ReferenceDataLoad load;
    std::array<std::optional<std::size_t>, 3> suppliedFrom;

    // The names come first, so a wrong list is reported before any file is read.
    for (std::size_t position = 0; position < aNames.size(); ++position)
    {
        const std::string& name = aNames[position];
        const auto found = std::find(names.begin(), names.end(), name);

        if (found == names.end())
        {
            load.mFault = ReferenceFileFault{
                .mFile = name,
                .mRow = std::nullopt,
                // A name without a file name is shown as a placeholder, not as two spaces.
                .mMessage = makeDiagnosticMessage(
                    MessageKey::ReferenceFileUnknownName,
                    {textParam("name", name.empty() ? "<no file name>" : name)}),
            };

            return load;
        }

        auto& slot = suppliedFrom[static_cast<std::size_t>(found - names.begin())];

        if (slot)
        {
            load.mFault = ReferenceFileFault{
                .mFile = name,
                .mRow = std::nullopt,
                .mMessage = makeDiagnosticMessage(MessageKey::ReferenceFileRepeatedName,
                                                  {textParam("name", name)}),
            };

            return load;
        }

        slot = position;
    }

    // The file being read or parsed, and its place in the supplied list (the log's source index).
    std::string_view currentFile;
    std::size_t currentPosition = 0;

    try
    {
        std::array<std::string, 3> suppliedTexts;

        for (std::size_t index = 0; index < names.size(); ++index)
        {
            if (!suppliedFrom[index])
            {
                continue;
            }

            currentFile = names[index];
            currentPosition = *suppliedFrom[index];

            auto text = aTextOf(*suppliedFrom[index]);

            if (const auto* fault = std::get_if<InputFileFault>(&text))
            {
                if (*fault == InputFileFault::Unreadable)
                {
                    failFile(names[index],
                             "the file cannot be read",
                             MessageKey::ReferenceFileUnreadable);
                }

                auto message = inputFileFaultMessage(*fault,
                                                     "reference_files",
                                                     currentPosition,
                                                     aMaxFileBytes);

                throw ReferenceFileError{std::string{names[index]} + ": the file was not read",
                                         ReferenceFileFault{.mFile = std::string{names[index]},
                                                            .mRow = std::nullopt,
                                                            .mMessage = std::move(message)}};
            }

            suppliedTexts[index] = std::move(std::get<std::string>(text));

            load.mOrigins[index] = CatalogueOrigin::Supplied;
        }

        // Only a supplied file is parsed; a list without one is the shared built-in list.
        const auto supplied = [&](std::size_t aIndex) {
            return load.mOrigins[aIndex] == CatalogueOrigin::Supplied;
        };

        currentFile = kPayersFile;
        currentPosition = suppliedFrom[0].value_or(0);

        const auto payers = supplied(0)
                                ? std::make_shared<const PayerList>(
                                      readPayers(suppliedTexts[0], CatalogueOrigin::Supplied))
                                : builtInLists().mPayers;

        currentFile = kReliefStatementsFile;
        currentPosition = suppliedFrom[1].value_or(0);

        const auto reliefStatements =
            supplied(1) ? std::make_shared<const std::vector<ReliefStatementEntry>>(
                              readReliefStatements(suppliedTexts[1], CatalogueOrigin::Supplied))
                        : builtInLists().mReliefStatements;

        currentFile = kBrokerPayersFile;
        currentPosition = suppliedFrom[2].value_or(0);

        const auto brokerPayers =
            supplied(2) ? std::make_shared<const BrokerPayerList>(
                              readBrokerPayers(suppliedTexts[2], CatalogueOrigin::Supplied))
                        : builtInLists().mBrokerPayers;

        load.mCatalogue = ReferenceCatalogue{payers, reliefStatements, brokerPayers};
    } catch (const ReferenceFileError& error)
    {
        load.mCatalogue.reset();
        load.mFault = error.fault();
    } catch (const std::bad_alloc&)
    {
        load.mCatalogue.reset();
        load.mFault = ReferenceFileFault{
            .mFile = std::string{currentFile},
            .mRow = std::nullopt,
            .mMessage = inputFileFaultMessage(InputFileFault::OutOfMemory,
                                              "reference_files",
                                              currentPosition,
                                              aMaxFileBytes),
        };
    }

    return load;
}

} // namespace

ReferenceDataLoad loadReferenceData(const std::span<const InputFile> aFiles,
                                    const std::size_t aMaxFileBytes) {
    std::vector<std::string> names;

    names.reserve(aFiles.size());

    for (const InputFile& file : aFiles)
    {
        // A display name with a folder part counts by its last part. A name without a file name
        // (empty or ending in a separator) is kept empty, so no folder reaches the fault.
        const auto filename = SourceFilename::tryFromPath(file.mName);

        names.push_back(filename ? filename->value() : std::string{});
    }

    return loadReferenceFiles(
        names,
        [&aFiles, aMaxFileBytes](std::size_t aPosition) -> ReferenceText {
            const std::string& content = aFiles[aPosition].mContent;

            if (content.size() > aMaxFileBytes)
            {
                return InputFileFault::TooLarge;
            }

            try
            { return std::string{content}; } catch (const std::bad_alloc&)
            { return InputFileFault::OutOfMemory; }
        },
        aMaxFileBytes);
}

ReferenceDataLoad loadReferenceData(const std::vector<std::filesystem::path>& aFiles,
                                    const std::size_t aMaxFileBytes) {
    std::vector<std::string> names(aFiles.size());

    std::transform(aFiles.begin(),
                   aFiles.end(),
                   names.begin(),
                   [](const std::filesystem::path& aPath) { return aPath.filename().string(); });

    return loadReferenceFiles(
        names,
        [&aFiles, aMaxFileBytes](std::size_t aPosition) -> ReferenceText {
            auto read = readInputFile(aFiles[aPosition], aMaxFileBytes);

            if (auto* fault = std::get_if<InputFileFault>(&read))
            {
                return *fault;
            }

            return std::move(std::get<InputFile>(read).mContent);
        },
        aMaxFileBytes);
}

} // namespace taxbroker
