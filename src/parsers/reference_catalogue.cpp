#include "taxbroker/reference_catalogue.hpp"

#include "utils/csv_text_reader.hpp"
#include "utils/date_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace taxbroker {

namespace {

constexpr std::string_view kPayersFile = "payers.csv";
constexpr std::string_view kReliefStatementsFile = "relief_statements.csv";
constexpr std::string_view kBrokerPayersFile = "broker_payers.csv";

constexpr std::array<std::string_view, 10> kPayerColumns{"isin",
                                                         "ibkr_conid",
                                                         "name",
                                                         "tax_number",
                                                         "address",
                                                         "country",
                                                         "instrument_kind",
                                                         "verified_on",
                                                         "source",
                                                         "source_revision"};
constexpr std::array<std::string_view, 4> kReliefStatementColumns{
    "country", "statement", "source", "source_revision"};
constexpr std::array<std::string_view, 8> kBrokerPayerColumns{
    "broker", "code", "name", "tax_number", "address", "country", "source", "source_revision"};

/// Throws the fault text: the file, the logical row and what is wrong, never a value.
[[noreturn]] void failRow(std::string_view aFile, std::size_t aRow, std::string_view aWhat) {
    throw std::logic_error{std::string{aFile} + " row " + std::to_string(aRow) + ": " +
                           std::string{aWhat}};
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
    CsvTable table = readCsvText(aText, ',', aRequiredColumns);

    if (!table.mFault)
    {
        return table;
    }

    const CsvFault& fault = *table.mFault;
    const std::string file{aFile};

    switch (fault.mKind)
    {
    case CsvFaultKind::UnclosedQuote:
        throw std::logic_error{file + ": a quote is not closed"};
    case CsvFaultKind::MissingColumns:
        throw std::logic_error{file + ": the header lacks the columns " +
                               joinColumns(fault.mColumns)};
    case CsvFaultKind::RepeatedColumn:
        throw std::logic_error{file + ": the header has the column " + joinColumns(fault.mColumns) +
                               " twice"};
    case CsvFaultKind::LineBreakInField:
        failRow(aFile, fault.mRowNumber.value_or(0), "a field holds a line break");
    case CsvFaultKind::FieldCountMismatch:
        failRow(aFile,
                fault.mRowNumber.value_or(0),
                "the row has " + std::to_string(fault.mFoundFields) + " fields, the header " +
                    std::to_string(fault.mExpectedFields));
    case CsvFaultKind::Unreadable:
        throw std::logic_error{file + ": the file cannot be read"};
    }

    throw std::logic_error{file + ": unknown fault"};
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

bool isDigitsOnly(std::string_view aValue) {
    return !aValue.empty() && std::all_of(aValue.begin(), aValue.end(), isDigit);
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
                           const std::string& aSourceRevision) {
    if (aSource != "ib-edavki" && aSource != "manual")
    {
        failRow(aFile, aRow, "the source is not ib-edavki or manual");
    }

    if (aSource == "ib-edavki" && aSourceRevision.empty())
    {
        failRow(aFile, aRow, "an ib-edavki row has no source revision");
    }

    return CatalogueSource{.mSource = aSource,
                           .mSourceRevision = aSourceRevision,
                           .mFile = std::string{aFile},
                           .mRowNumber = aRow};
}

void requireText(std::string_view aFile,
                 std::size_t aRow,
                 const std::string& aValue,
                 std::string_view aWhat) {
    if (aValue.empty())
    {
        failRow(aFile, aRow, aWhat);
    }
}

void requireCountry(std::string_view aFile, std::size_t aRow, const std::string& aValue) {
    if (!isCountry(aValue))
    {
        failRow(aFile, aRow, "the country is not 2 uppercase letters");
    }
}

std::vector<PayerEntry> readPayers(std::string_view aText) {
    const CsvTable table = readFile(kPayersFile, aText, kPayerColumns);
    const std::size_t isinColumn = table.columnIndex("isin");
    const std::size_t conidColumn = table.columnIndex("ibkr_conid");
    const std::size_t nameColumn = table.columnIndex("name");
    const std::size_t taxNumberColumn = table.columnIndex("tax_number");
    const std::size_t addressColumn = table.columnIndex("address");
    const std::size_t countryColumn = table.columnIndex("country");
    const std::size_t kindColumn = table.columnIndex("instrument_kind");
    const std::size_t verifiedColumn = table.columnIndex("verified_on");
    const std::size_t sourceColumn = table.columnIndex("source");
    const std::size_t revisionColumn = table.columnIndex("source_revision");

    std::vector<PayerEntry> payers;

    for (const CsvRow& row : table.mRows)
    {
        const std::size_t number = row.mRowNumber;
        const auto& fields = row.mFields;

        if (fields[isinColumn].empty() && fields[conidColumn].empty())
        {
            failRow(kPayersFile, number, "the row has neither an ISIN nor a conid");
        }

        if (!fields[isinColumn].empty() && !isValidIsin(fields[isinColumn]))
        {
            failRow(kPayersFile, number, "the ISIN is not valid");
        }

        if (!fields[conidColumn].empty() && !isDigitsOnly(fields[conidColumn]))
        {
            failRow(kPayersFile, number, "the conid is not digits only");
        }

        requireText(kPayersFile, number, fields[nameColumn], "the name is empty");
        requireText(kPayersFile, number, fields[addressColumn], "the address is empty");
        requireCountry(kPayersFile, number, fields[countryColumn]);

        const std::string& kind = fields[kindColumn];

        if (!kind.empty() && kind != "share" && kind != "etf" && kind != "fund" && kind != "bond")
        {
            failRow(kPayersFile, number, "the instrument kind is not share, etf, fund or bond");
        }

        std::optional<Date> verifiedOn;

        if (!fields[verifiedColumn].empty())
        {
            verifiedOn = parseCalendarDate(fields[verifiedColumn]);

            if (!verifiedOn)
            {
                failRow(kPayersFile, number, "verified_on is not a date as YYYY-MM-DD");
            }
        }

        payers.push_back(PayerEntry{
            .mIsin = optionalText(fields[isinColumn]),
            .mIbkrConid = optionalText(fields[conidColumn]),
            .mName = fields[nameColumn],
            .mTaxNumber = optionalText(fields[taxNumberColumn]),
            .mAddress = fields[addressColumn],
            .mCountry = fields[countryColumn],
            .mInstrumentKind = optionalText(kind),
            .mVerifiedOn = verifiedOn,
            .mSource =
                readSource(kPayersFile, number, fields[sourceColumn], fields[revisionColumn]),
        });
    }

    return payers;
}

std::vector<ReliefStatementEntry> readReliefStatements(std::string_view aText) {
    const CsvTable table = readFile(kReliefStatementsFile, aText, kReliefStatementColumns);
    const std::size_t countryColumn = table.columnIndex("country");
    const std::size_t statementColumn = table.columnIndex("statement");
    const std::size_t sourceColumn = table.columnIndex("source");
    const std::size_t revisionColumn = table.columnIndex("source_revision");

    std::vector<ReliefStatementEntry> statements;

    for (const CsvRow& row : table.mRows)
    {
        const std::size_t number = row.mRowNumber;
        const auto& fields = row.mFields;

        requireCountry(kReliefStatementsFile, number, fields[countryColumn]);
        requireText(kReliefStatementsFile,
                    number,
                    fields[statementColumn],
                    "the statement is empty");

        statements.push_back(ReliefStatementEntry{
            .mCountry = fields[countryColumn],
            .mStatement = fields[statementColumn],
            .mSource = readSource(kReliefStatementsFile,
                                  number,
                                  fields[sourceColumn],
                                  fields[revisionColumn]),
        });
    }

    return statements;
}

std::vector<BrokerPayerEntry> readBrokerPayers(std::string_view aText) {
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

        requireText(kBrokerPayersFile, number, fields[brokerColumn], "the broker is empty");
        requireText(kBrokerPayersFile, number, fields[codeColumn], "the code is empty");
        requireText(kBrokerPayersFile, number, fields[nameColumn], "the name is empty");
        requireText(kBrokerPayersFile, number, fields[addressColumn], "the address is empty");
        requireCountry(kBrokerPayersFile, number, fields[countryColumn]);

        const bool repeated = std::any_of(brokerPayers.begin(),
                                          brokerPayers.end(),
                                          [&](const BrokerPayerEntry& aEntry) {
                                              return aEntry.mBroker == fields[brokerColumn] &&
                                                     aEntry.mCode == fields[codeColumn];
                                          });

        if (repeated)
        {
            failRow(kBrokerPayersFile, number, "the broker and code are already in the file");
        }

        brokerPayers.push_back(BrokerPayerEntry{
            .mBroker = fields[brokerColumn],
            .mCode = fields[codeColumn],
            .mName = fields[nameColumn],
            .mTaxNumber = optionalText(fields[taxNumberColumn]),
            .mAddress = fields[addressColumn],
            .mCountry = fields[countryColumn],
            .mSource =
                readSource(kBrokerPayersFile, number, fields[sourceColumn], fields[revisionColumn]),
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

ReferenceCatalogue::ReferenceCatalogue(std::vector<PayerEntry> aPayers,
                                       std::vector<ReliefStatementEntry> aReliefStatements,
                                       std::vector<BrokerPayerEntry> aBrokerPayers)
    : mPayers(std::move(aPayers)), mReliefStatements(std::move(aReliefStatements)),
      mBrokerPayers(std::move(aBrokerPayers)) {}

PayerLookup ReferenceCatalogue::findPayer(const PayerQuery& aQuery) const {
    std::vector<const PayerEntry*> matches;

    for (const PayerEntry& entry : mPayers)
    {
        const bool sameIsin = aQuery.mIsin && entry.mIsin == aQuery.mIsin;
        const bool sameConid = aQuery.mIbkrConid && entry.mIbkrConid == aQuery.mIbkrConid;

        if (sameIsin || sameConid)
        {
            matches.push_back(&entry);
        }
    }

    if (matches.empty())
    {
        return PayerLookup{};
    }

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
ReferenceCatalogue::reliefStatements(std::string_view aCountry) const {
    std::vector<const ReliefStatementEntry*> statements;

    for (const ReliefStatementEntry& entry : mReliefStatements)
    {
        if (entry.mCountry == aCountry)
        {
            statements.push_back(&entry);
        }
    }

    return statements;
}

const BrokerPayerEntry* ReferenceCatalogue::brokerPayer(std::string_view aBroker,
                                                        std::string_view aCode) const {
    const auto found = std::find_if(mBrokerPayers.begin(),
                                    mBrokerPayers.end(),
                                    [&](const BrokerPayerEntry& aEntry) {
                                        return aEntry.mBroker == aBroker && aEntry.mCode == aCode;
                                    });

    return found == mBrokerPayers.end() ? nullptr : &*found;
}

const std::vector<PayerEntry>& ReferenceCatalogue::payers() const {
    return mPayers;
}

const std::vector<ReliefStatementEntry>& ReferenceCatalogue::allReliefStatements() const {
    return mReliefStatements;
}

const std::vector<BrokerPayerEntry>& ReferenceCatalogue::brokerPayers() const {
    return mBrokerPayers;
}

ReferenceCatalogue parseReferenceCatalogue(std::string_view aPayersCsv,
                                           std::string_view aReliefStatementsCsv,
                                           std::string_view aBrokerPayersCsv) {
    return ReferenceCatalogue{readPayers(aPayersCsv),
                              readReliefStatements(aReliefStatementsCsv),
                              readBrokerPayers(aBrokerPayersCsv)};
}

const ReferenceCatalogue& referenceCatalogue() {
    static const ReferenceCatalogue catalogue =
        parseReferenceCatalogue(embeddedPayersCsv(),
                                embeddedReliefStatementsCsv(),
                                embeddedBrokerPayersCsv());

    return catalogue;
}

} // namespace taxbroker
