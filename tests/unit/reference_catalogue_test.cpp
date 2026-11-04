#include "taxbroker/reference_catalogue.hpp"

#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace taxbroker;

// The synthetic ISINs use the prefix XX (X counts as 33). Check digit by the Luhn rule on the
// expanded digits: XX000000001 -> 3333 000000001. With the check digit last, the digits counted
// from the right are 1 (doubled: 2), 3, 3 (doubled: 6), 3, 3 (doubled: 6): 2 + 3 + 6 + 3 + 6 = 20,
// a multiple of 10, so the check digit is 0.
constexpr std::string_view kIsinA = "XX0000000010";
constexpr std::string_view kIsinB = "XX0000000028"; // same prefix as A, another instrument

constexpr std::string_view kPayersHeader =
    "isin,ibkr_conid,name,tax_number,address,country,instrument_kind,verified_on,source,"
    "source_revision\n";
constexpr std::string_view kReliefHeader = "country,statement,source,source_revision\n";
constexpr std::string_view kBrokerHeader =
    "broker,code,name,tax_number,address,country,source,source_revision\n";

// One valid row each, for the tests that break only one of the other files.
constexpr std::string_view kOneRelief = "BE,Treaty text,manual,\n";
constexpr std::string_view kOneBroker = "IBKR,IB-X,Broker X,123,1 Main Street,GB,manual,\n";

ReferenceCatalogue
parse(std::string_view aPayers, std::string_view aRelief = {}, std::string_view aBroker = {}) {
    const std::string payers = std::string{kPayersHeader} + std::string{aPayers};
    const std::string relief =
        std::string{kReliefHeader} + std::string{aRelief.empty() ? kOneRelief : aRelief};
    const std::string broker =
        std::string{kBrokerHeader} + std::string{aBroker.empty() ? kOneBroker : aBroker};

    return parseReferenceCatalogue(payers, relief, broker);
}

// --- The embedded files ---

TEST(ReferenceCatalogueTest, LoadsTheEmbeddedFiles) {
    const ReferenceCatalogue& catalogue = referenceCatalogue();

    EXPECT_EQ(catalogue.payers().size(), 299U); // 300 upstream payers, one ISIN is not valid
    EXPECT_EQ(catalogue.allReliefStatements().size(), 17U);
    EXPECT_EQ(catalogue.brokerPayers().size(), 5U);
}

TEST(ReferenceCatalogueTest, EmbeddedPayersHaveNoKindNoVerificationDateAndTheUpstreamSource) {
    const ReferenceCatalogue& catalogue = referenceCatalogue();

    for (const PayerEntry& entry : catalogue.payers())
    {
        EXPECT_FALSE(entry.mInstrumentKind.has_value());
        EXPECT_FALSE(entry.mVerifiedOn.has_value());
        EXPECT_EQ(entry.mSource.mSource, "ib-edavki");
        EXPECT_EQ(entry.mSource.mFile, "payers.csv");
    }
}

// --- Lookup ---

TEST(ReferenceCatalogueTest, FindsAPayerByExactIsin) {
    const auto catalogue =
        parse("XX0000000010,,Payer A,TAX-A,1 Main Street,IE,etf,2026-01-15,manual,\n");

    const PayerLookup lookup = catalogue.findPayer({.mIsin = std::string{kIsinA}});

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Found);
    ASSERT_EQ(lookup.mEntries.size(), 1U);

    const PayerEntry& entry = *lookup.mEntries[0];

    EXPECT_EQ(entry.mName, "Payer A");
    EXPECT_EQ(entry.mTaxNumber, std::optional<std::string>{"TAX-A"});
    EXPECT_EQ(entry.mAddress, "1 Main Street");
    EXPECT_EQ(entry.mCountry, "IE");
    EXPECT_EQ(entry.mInstrumentKind, std::optional<std::string>{"etf"});
    EXPECT_EQ(entry.mVerifiedOn, parseCalendarDate("2026-01-15"));
    EXPECT_EQ(entry.mSource.mSource, "manual");
    EXPECT_EQ(entry.mSource.mFile, "payers.csv");
    EXPECT_EQ(entry.mSource.mRowNumber, 2U); // the header is row 1
}

TEST(ReferenceCatalogueTest, FindsAPayerByExactConid) {
    const auto catalogue = parse(",12345,Payer C,,2 Main Street,US,,,ib-edavki,abc123\n");

    const PayerLookup lookup = catalogue.findPayer({.mIbkrConid = std::string{"12345"}});

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Found);
    ASSERT_EQ(lookup.mEntries.size(), 1U);
    EXPECT_EQ(lookup.mEntries[0]->mName, "Payer C");
    EXPECT_FALSE(lookup.mEntries[0]->mTaxNumber.has_value()); // the field is empty
    EXPECT_EQ(lookup.mEntries[0]->mSource.mSourceRevision, "abc123");
}

TEST(ReferenceCatalogueTest, DoesNotMatchAnotherIsinWithTheSamePrefix) {
    const auto catalogue = parse("XX0000000010,,Payer A,,1 Main Street,IE,,,manual,\n");

    const PayerLookup lookup = catalogue.findPayer({.mIsin = std::string{kIsinB}});

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::NotFound);
    EXPECT_TRUE(lookup.mEntries.empty());
}

TEST(ReferenceCatalogueTest, DoesNotMatchAPartOfAConid) {
    const auto catalogue = parse(",12345,Payer C,,2 Main Street,US,,,manual,\n");

    const PayerLookup lookup = catalogue.findPayer({.mIbkrConid = std::string{"1234"}});

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::NotFound);
}

TEST(ReferenceCatalogueTest, DoesNotMatchAConidAsAnIsin) {
    const auto catalogue = parse(",12345,Payer C,,2 Main Street,US,,,manual,\n");

    const PayerLookup lookup = catalogue.findPayer({.mIsin = std::string{"12345"}});

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::NotFound);
}

TEST(ReferenceCatalogueTest, ReturnsNotFoundForAQueryWithoutIdentifiers) {
    const auto catalogue = parse("XX0000000010,,Payer A,,1 Main Street,IE,,,manual,\n");

    const PayerLookup lookup = catalogue.findPayer({});

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::NotFound);
}

TEST(ReferenceCatalogueTest, CountsARowFoundByIsinAndByConidOnce) {
    const auto catalogue = parse("XX0000000010,7,Payer A,,1 Main Street,IE,,,manual,\n");

    const PayerLookup lookup =
        catalogue.findPayer({.mIsin = std::string{kIsinA}, .mIbkrConid = std::string{"7"}});

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Found);
    EXPECT_EQ(lookup.mEntries.size(), 1U);
}

TEST(ReferenceCatalogueTest, ReportsAConflictWhenIsinAndConidNameDifferentPayers) {
    const auto catalogue = parse("XX0000000010,,Payer X,,1 Main Street,IE,,,manual,\n"
                                 ",7,Payer Y,,2 Main Street,IE,,,manual,\n");

    const PayerLookup lookup =
        catalogue.findPayer({.mIsin = std::string{kIsinA}, .mIbkrConid = std::string{"7"}});

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Conflict);
    ASSERT_EQ(lookup.mEntries.size(), 2U);
    EXPECT_EQ(lookup.mEntries[0]->mSource.mRowNumber, 2U);
    EXPECT_EQ(lookup.mEntries[1]->mSource.mRowNumber, 3U);
}

TEST(ReferenceCatalogueTest, ReportsAConflictForTwoRowsOfOneIsinWithAnotherCountry) {
    const auto catalogue = parse("XX0000000010,,Payer A,,1 Main Street,IE,,,manual,\n"
                                 "XX0000000010,,Payer A,,1 Main Street,LU,,,manual,\n");

    const PayerLookup lookup = catalogue.findPayer({.mIsin = std::string{kIsinA}});

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Conflict);
    EXPECT_EQ(lookup.mEntries.size(), 2U);
}

TEST(ReferenceCatalogueTest, TreatsTwoRowsWithTheSameFactsAsOne) {
    const auto catalogue =
        parse("XX0000000010,,Payer A,TAX-A,1 Main Street,IE,,,ib-edavki,abc123\n"
              "XX0000000010,,Payer A,TAX-A,1 Main Street,IE,,2026-01-15,manual,\n");

    const PayerLookup lookup = catalogue.findPayer({.mIsin = std::string{kIsinA}});

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Found);
    ASSERT_EQ(lookup.mEntries.size(), 1U);
    EXPECT_EQ(lookup.mEntries[0]->mSource.mRowNumber, 2U); // the first row in file order
}

TEST(ReferenceCatalogueTest, KeepsKindAndVerificationDateOfAManualRow) {
    const auto catalogue =
        parse("XX0000000036,,Fund F,,3 Main Street,LU,fund,2026-01-15,manual,\n");

    const PayerLookup lookup = catalogue.findPayer({.mIsin = std::string{"XX0000000036"}});

    ASSERT_EQ(lookup.mEntries.size(), 1U);
    EXPECT_EQ(lookup.mEntries[0]->mInstrumentKind, std::optional<std::string>{"fund"});
    EXPECT_EQ(lookup.mEntries[0]->mVerifiedOn, parseCalendarDate("2026-01-15"));
}

TEST(ReferenceCatalogueTest, FindsAnIsinWithALetterBeforeTheCheckDigit) {
    // XX00000000A -> 3333 00000000 10 (A counts as 10, two digits, which moves the doubling).
    // With the check digit last, the non-zero digits from the right are 1, 3 (doubled: 6), 3,
    // 3 (doubled: 6), 3: 1 + 6 + 3 + 6 + 3 = 19, so the check digit is 1 (sum 20).
    const auto catalogue = parse("XX00000000A1,,Payer L,,4 Main Street,IE,,,manual,\n");

    const PayerLookup lookup = catalogue.findPayer({.mIsin = std::string{"XX00000000A1"}});

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Found);
    ASSERT_EQ(lookup.mEntries.size(), 1U);
    EXPECT_EQ(lookup.mEntries[0]->mName, "Payer L");
}

TEST(ReferenceCatalogueTest, ReadsAQuotedAddressWithAComma) {
    const auto catalogue =
        parse("XX0000000010,,Payer A,,\"Level 20, 1 Main Street\",IE,,,manual,\n");

    const PayerLookup lookup = catalogue.findPayer({.mIsin = std::string{kIsinA}});

    ASSERT_EQ(lookup.mEntries.size(), 1U);
    EXPECT_EQ(lookup.mEntries[0]->mAddress, "Level 20, 1 Main Street");
}

TEST(ReferenceCatalogueTest, FindsABrokerPayerByBrokerAndCode) {
    const auto catalogue = parse("XX0000000010,,Payer A,,1 Main Street,IE,,,manual,\n",
                                 "BE,Treaty text,manual,\n",
                                 "IBKR,IB-UK,Broker UK,03958476,Level 20,GB,ib-edavki,abc123\n");

    const BrokerPayerEntry* found = catalogue.brokerPayer("IBKR", "IB-UK");

    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->mName, "Broker UK");
    EXPECT_EQ(found->mTaxNumber, std::optional<std::string>{"03958476"});
    EXPECT_EQ(found->mSource.mFile, "broker_payers.csv");
    EXPECT_EQ(catalogue.brokerPayer("TR", "IB-UK"), nullptr);   // another broker
    EXPECT_EQ(catalogue.brokerPayer("IBKR", "IB-XX"), nullptr); // another code
}

TEST(ReferenceCatalogueTest, ListsTheReliefStatementsOfACountryInFileOrder) {
    const auto catalogue =
        parse("XX0000000010,,Payer A,,1 Main Street,IE,,,manual,\n",
              "BE,Text one,manual,\nCA,Text two,manual,\nBE,Text three,manual,\n");

    const auto belgium = catalogue.reliefStatements("BE");

    ASSERT_EQ(belgium.size(), 2U);
    EXPECT_EQ(belgium[0]->mStatement, "Text one");
    EXPECT_EQ(belgium[1]->mStatement, "Text three");
    EXPECT_EQ(catalogue.reliefStatements("CA").size(), 1U);
    EXPECT_TRUE(catalogue.reliefStatements("DE").empty());
}

// --- Load faults ---

void expectFault(std::string_view aPayersRows,
                 std::string_view aExpectedMessage,
                 std::string_view aRelief = {},
                 std::string_view aBroker = {}) {
    try
    {
        static_cast<void>(parse(aPayersRows, aRelief, aBroker));
        FAIL() << "No fault for the expected message: " << aExpectedMessage;
    } catch (const std::logic_error& error)
    { EXPECT_EQ(std::string{error.what()}, std::string{aExpectedMessage}); }
}

TEST(ReferenceCatalogueTest, RejectsARowWithoutIsinAndConid) {
    // Row 2 has no ISIN and no conid.
    expectFault(",,Payer A,,1 Main Street,IE,,,manual,\n",
                "payers.csv row 2: the row has neither an ISIN nor a conid");
}

TEST(ReferenceCatalogueTest, RejectsAnIsinWithAWrongCheckDigit) {
    // XX0000000011: the check digit of XX000000001 is 0.
    expectFault("XX0000000011,,Payer A,,1 Main Street,IE,,,manual,\n",
                "payers.csv row 2: the ISIN is not valid");
}

TEST(ReferenceCatalogueTest, RejectsAnIsinWithALetterAndAWrongCheckDigit) {
    // XX00000000A0: the check digit of XX00000000A is 1.
    expectFault("XX00000000A0,,Payer L,,4 Main Street,IE,,,manual,\n",
                "payers.csv row 2: the ISIN is not valid");
}

TEST(ReferenceCatalogueTest, RejectsAnIsinOfWrongLength) {
    // 11 characters.
    expectFault("XX000000001,,Payer A,,1 Main Street,IE,,,manual,\n",
                "payers.csv row 2: the ISIN is not valid");
}

TEST(ReferenceCatalogueTest, RejectsAnIsinWithALowercaseLetter) {
    expectFault("xx0000000010,,Payer A,,1 Main Street,IE,,,manual,\n",
                "payers.csv row 2: the ISIN is not valid");
}

TEST(ReferenceCatalogueTest, RejectsAConidWithALetter) {
    expectFault(",12A45,Payer A,,1 Main Street,IE,,,manual,\n",
                "payers.csv row 2: the conid is not digits only");
}

TEST(ReferenceCatalogueTest, RejectsAThreeLetterCountry) {
    expectFault(",7,Payer A,,1 Main Street,IRL,,,manual,\n",
                "payers.csv row 2: the country is not 2 uppercase letters");
}

TEST(ReferenceCatalogueTest, RejectsAnEmptyName) {
    expectFault(",7,,,1 Main Street,IE,,,manual,\n", "payers.csv row 2: the name is empty");
}

TEST(ReferenceCatalogueTest, RejectsAnEmptyAddress) {
    expectFault(",7,Payer A,,,IE,,,manual,\n", "payers.csv row 2: the address is empty");
}

TEST(ReferenceCatalogueTest, RejectsAnUnknownSource) {
    expectFault(",7,Payer A,,1 Main Street,IE,,,other,\n",
                "payers.csv row 2: the source is not ib-edavki or manual");
}

TEST(ReferenceCatalogueTest, RejectsAnUpstreamRowWithoutRevision) {
    expectFault(",7,Payer A,,1 Main Street,IE,,,ib-edavki,\n",
                "payers.csv row 2: an ib-edavki row has no source revision");
}

TEST(ReferenceCatalogueTest, RejectsAnInvalidVerificationDate) {
    // Month 13.
    expectFault(",7,Payer A,,1 Main Street,IE,,2026-13-01,manual,\n",
                "payers.csv row 2: verified_on is not a date as YYYY-MM-DD");
}

TEST(ReferenceCatalogueTest, RejectsAnUnknownInstrumentKind) {
    expectFault(",7,Payer A,,1 Main Street,IE,crypto,,manual,\n",
                "payers.csv row 2: the instrument kind is not share, etf, fund or bond");
}

TEST(ReferenceCatalogueTest, ReportsTheLogicalRowOfTheFault) {
    // Row 2 is valid; row 3 has no name.
    expectFault(",7,Payer A,,1 Main Street,IE,,,manual,\n,8,,,2 Main Street,IE,,,manual,\n",
                "payers.csv row 3: the name is empty");
}

TEST(ReferenceCatalogueTest, RejectsARowWithTooFewFields) {
    // 9 fields: source_revision is missing.
    expectFault(",7,Payer A,,1 Main Street,IE,,,manual\n",
                "payers.csv row 2: the row has 9 fields, the header 10");
}

TEST(ReferenceCatalogueTest, RejectsAPayersFileWithoutAColumn) {
    // The header lacks instrument_kind and verified_on.
    const std::string payers =
        "isin,ibkr_conid,name,tax_number,address,country,source,source_revision\n";

    try
    {
        static_cast<void>(
            parseReferenceCatalogue(payers,
                                    std::string{kReliefHeader} + std::string{kOneRelief},
                                    std::string{kBrokerHeader} + std::string{kOneBroker}));
        FAIL() << "No fault";
    } catch (const std::logic_error& error)
    {
        EXPECT_EQ(std::string{error.what()},
                  "payers.csv: the header lacks the columns instrument_kind, verified_on");
    }
}

TEST(ReferenceCatalogueTest, RejectsAnIsinWithASymbol) {
    // 12 characters, but the 7th is a hyphen.
    expectFault("XX0000-00010,,Payer A,,1 Main Street,IE,,,manual,\n",
                "payers.csv row 2: the ISIN is not valid");
}

TEST(ReferenceCatalogueTest, RejectsAPayersFileWithARepeatedColumn) {
    // The header has the column name twice.
    const std::string payers =
        "isin,ibkr_conid,name,tax_number,address,country,instrument_kind,verified_on,source,"
        "source_revision,name\n";

    try
    {
        static_cast<void>(
            parseReferenceCatalogue(payers,
                                    std::string{kReliefHeader} + std::string{kOneRelief},
                                    std::string{kBrokerHeader} + std::string{kOneBroker}));
        FAIL() << "No fault";
    } catch (const std::logic_error& error)
    { EXPECT_EQ(std::string{error.what()}, "payers.csv: the header has the column name twice"); }
}

TEST(ReferenceCatalogueTest, RejectsAFieldWithALineBreak) {
    // The address starts a quote on row 2 and closes it on the next line; the quotes are even.
    expectFault(",7,Payer A,,\"Level 20\n1 Main Street\",IE,,,manual,\n",
                "payers.csv row 2: a field holds a line break");
}

TEST(ReferenceCatalogueTest, RejectsAPayersFileWithAnUnclosedQuote) {
    // The quote before Level never closes.
    expectFault(",7,Payer A,,\"Level 20,IE,,,manual,\n", "payers.csv: a quote is not closed");
}

TEST(ReferenceCatalogueTest, RejectsAnEmptyReliefStatement) {
    expectFault(",7,Payer A,,1 Main Street,IE,,,manual,\n",
                "relief_statements.csv row 2: the statement is empty",
                "BE,,manual,\n");
}

TEST(ReferenceCatalogueTest, RejectsAReliefStatementWithAWrongCountry) {
    expectFault(",7,Payer A,,1 Main Street,IE,,,manual,\n",
                "relief_statements.csv row 2: the country is not 2 uppercase letters",
                "Belgium,Treaty text,manual,\n");
}

TEST(ReferenceCatalogueTest, RejectsARepeatedBrokerPayer) {
    // Rows 2 and 3 have the broker IBKR and the code IB-X.
    expectFault(",7,Payer A,,1 Main Street,IE,,,manual,\n",
                "broker_payers.csv row 3: the broker and code are already in the file",
                "BE,Treaty text,manual,\n",
                "IBKR,IB-X,Broker X,123,1 Main Street,GB,manual,\n"
                "IBKR,IB-X,Broker Y,456,2 Main Street,GB,manual,\n");
}

TEST(ReferenceCatalogueTest, RejectsABrokerPayerWithoutACode) {
    expectFault(",7,Payer A,,1 Main Street,IE,,,manual,\n",
                "broker_payers.csv row 2: the code is empty",
                "BE,Treaty text,manual,\n",
                "IBKR,,Broker X,123,1 Main Street,GB,manual,\n");
}

TEST(ReferenceCatalogueTest, DoesNotPutAValueOfTheFileInAFaultMessage) {
    try
    {
        static_cast<void>(parse(",7,Secret Name,,1 Main Street,IRL,,,manual,\n"));
        FAIL() << "No fault";
    } catch (const std::logic_error& error)
    {
        EXPECT_EQ(std::string{error.what()}.find("Secret Name"), std::string::npos);
        EXPECT_EQ(std::string{error.what()}.find("IRL"), std::string::npos);
    }
}

// --- Supplied files ---

/// A path under the system temp directory that no other test of this run uses.
std::filesystem::path uniqueFolderPath() {
    static std::atomic<unsigned> counter{0};

    return std::filesystem::temp_directory_path() /
           ("taxbroker_reference_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(counter++));
}

/// A new empty folder under the system temp directory, removed with the object.
class TemporaryFolder {
  public:
    TemporaryFolder() : mPath{uniqueFolderPath()} {
        std::filesystem::create_directories(mPath);
    }

    ~TemporaryFolder() {
        std::error_code ignored;

        std::filesystem::remove_all(mPath, ignored);
    }

    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;

    /// Writes the text as the file `aName` (in a subfolder when given) and returns its path.
    [[nodiscard]] std::filesystem::path
    write(std::string_view aName, std::string_view aText, std::string_view aSubfolder = {}) const {
        const auto folder = mPath / aSubfolder;

        std::filesystem::create_directories(folder);

        const auto path = folder / aName;
        std::ofstream output{path, std::ios::binary};

        output << aText;

        return path;
    }

    [[nodiscard]] std::filesystem::path path(std::string_view aName) const {
        return mPath / aName;
    }

  private:
    std::filesystem::path mPath;
};

/// A valid payers file with one row for the ISIN A.
const std::string kSuppliedPayers =
    std::string{kPayersHeader} + "XX0000000010,,Payer A,,1 Main Street,IE,,,manual,\n";

/// Loads one supplied payers file with this text and returns the fault, which must be set.
ReferenceFileFault payersFault(std::string_view aText) {
    TemporaryFolder folder;
    const auto load = loadReferenceData({folder.write("payers.csv", aText)});

    EXPECT_FALSE(load.mCatalogue.has_value());
    EXPECT_TRUE(load.mFault.has_value());

    return load.mFault.value_or(ReferenceFileFault{});
}

TEST(ReferenceDataTest, NoFilesGiveTheBuiltInData) {
    const auto load = loadReferenceData({});

    ASSERT_TRUE(load.mCatalogue.has_value());
    EXPECT_FALSE(load.mFault.has_value());
    EXPECT_EQ(load.mCatalogue->payers().size(), 299U);
    EXPECT_EQ(load.mCatalogue->allReliefStatements().size(), 17U);
    EXPECT_EQ(load.mOrigins[0], CatalogueOrigin::BuiltIn);
    EXPECT_EQ(load.mOrigins[1], CatalogueOrigin::BuiltIn);
    EXPECT_EQ(load.mOrigins[2], CatalogueOrigin::BuiltIn);
    EXPECT_EQ(load.mCatalogue->payers().front().mSource.mOrigin, CatalogueOrigin::BuiltIn);
}

TEST(ReferenceDataTest, ASuppliedPayersFileReplacesTheBuiltInFileWhole) {
    TemporaryFolder folder;
    const auto load = loadReferenceData({folder.write("payers.csv", kSuppliedPayers)});

    ASSERT_TRUE(load.mCatalogue.has_value());
    EXPECT_EQ(load.mOrigins[0], CatalogueOrigin::Supplied);
    EXPECT_EQ(load.mOrigins[1], CatalogueOrigin::BuiltIn);
    EXPECT_EQ(load.mOrigins[2], CatalogueOrigin::BuiltIn);
    ASSERT_EQ(load.mCatalogue->payers().size(), 1U); // the 299 built-in payers are gone

    const auto found = load.mCatalogue->findPayer(PayerQuery{.mIsin = Isin{std::string{kIsinA}}});

    ASSERT_EQ(found.mStatus, PayerLookupStatus::Found);
    EXPECT_EQ(found.mEntries.front()->mName, "Payer A");
    EXPECT_EQ(found.mEntries.front()->mSource.mOrigin, CatalogueOrigin::Supplied);
    EXPECT_EQ(found.mEntries.front()->mSource.mFile, "payers.csv");
    EXPECT_EQ(found.mEntries.front()->mSource.mRowNumber, 2U); // the header is row 1

    // An ISIN of the built-in file is not found in the supplied one.
    const auto& builtInPayers = referenceCatalogue().payers();
    const auto withIsin =
        std::find_if(builtInPayers.begin(), builtInPayers.end(), [](const PayerEntry& aPayer) {
            return aPayer.mIsin.has_value();
        });

    ASSERT_NE(withIsin, builtInPayers.end());

    const auto missing = load.mCatalogue->findPayer(PayerQuery{.mIsin = *withIsin->mIsin});

    EXPECT_EQ(missing.mStatus, PayerLookupStatus::NotFound);

    // The other two files stay built in.
    EXPECT_EQ(load.mCatalogue->allReliefStatements().size(), 17U);
    EXPECT_EQ(load.mCatalogue->allReliefStatements().front().mSource.mOrigin,
              CatalogueOrigin::BuiltIn);
}

TEST(ReferenceDataTest, EachSuppliedFileReplacesOnlyTheFileOfItsName) {
    TemporaryFolder folder;
    const auto load = loadReferenceData(
        {folder.write("relief_statements.csv",
                      std::string{kReliefHeader} + "BE,Treaty text,manual,\n"),
         folder.write("broker_payers.csv", std::string{kBrokerHeader} + std::string{kOneBroker})});

    ASSERT_TRUE(load.mCatalogue.has_value());
    EXPECT_EQ(load.mOrigins[0], CatalogueOrigin::BuiltIn);
    EXPECT_EQ(load.mOrigins[1], CatalogueOrigin::Supplied);
    EXPECT_EQ(load.mOrigins[2], CatalogueOrigin::Supplied);

    EXPECT_EQ(load.mCatalogue->payers().size(), 299U);

    ASSERT_EQ(load.mCatalogue->allReliefStatements().size(), 1U);
    EXPECT_EQ(load.mCatalogue->allReliefStatements().front().mStatement, "Treaty text");

    ASSERT_EQ(load.mCatalogue->brokerPayers().size(), 1U);
    EXPECT_EQ(load.mCatalogue->brokerPayers().front().mCode, "IB-X");
}

TEST(ReferenceDataTest, AFileNameThatIsNotOneOfTheThreeIsAFault) {
    TemporaryFolder folder;
    const auto load = loadReferenceData({folder.write("payer.csv", kSuppliedPayers)});

    EXPECT_FALSE(load.mCatalogue.has_value());
    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "payer.csv");
    EXPECT_EQ(load.mFault->mRow, std::nullopt);
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileUnknownName);
    EXPECT_EQ(load.mFault->mMessage.mParams,
              (std::vector<MessageParam>{textParam("name", "payer.csv")}));
}

TEST(ReferenceDataTest, TheNameIsCheckedBeforeAnyFileIsRead) {
    // payers.csv does not exist, but the wrong name of the second file is reported first.
    TemporaryFolder folder;
    const auto load =
        loadReferenceData({folder.path("payers.csv"), folder.write("notes.txt", "x")});

    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileUnknownName);
    EXPECT_EQ(load.mFault->mFile, "notes.txt");
}

TEST(ReferenceDataTest, TheSameFileNameTwiceIsAFault) {
    TemporaryFolder folder;
    const auto load = loadReferenceData({folder.write("payers.csv", kSuppliedPayers, "first"),
                                         folder.write("payers.csv", kSuppliedPayers, "second")});

    EXPECT_FALSE(load.mCatalogue.has_value());
    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "payers.csv");
    EXPECT_EQ(load.mFault->mRow, std::nullopt);
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileRepeatedName);
}

TEST(ReferenceDataTest, AFileThatCannotBeOpenedIsAFault) {
    TemporaryFolder folder;
    const auto load = loadReferenceData({folder.path("payers.csv")});

    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "payers.csv");
    EXPECT_EQ(load.mFault->mRow, std::nullopt);
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileUnreadable);
}

TEST(ReferenceDataTest, AFolderNamedPayersCsvIsAFault) {
    TemporaryFolder folder;
    const auto payersFolder = folder.path("payers.csv");

    std::filesystem::create_directories(payersFolder);

    const auto load = loadReferenceData({payersFolder});

    EXPECT_FALSE(load.mCatalogue.has_value());
    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "payers.csv");
    EXPECT_EQ(load.mFault->mRow, std::nullopt);
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileUnreadable);
}

TEST(ReferenceDataTest, FaultOfARowWithoutAnyIdentifier) {
    const auto fault =
        payersFault(std::string{kPayersHeader} + ",,Payer A,,1 Main Street,IE,,,manual,\n");

    EXPECT_EQ(fault.mFile, "payers.csv");
    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileNoIdentifier);
    EXPECT_TRUE(fault.mMessage.mParams.empty());
}

TEST(ReferenceDataTest, FaultOfAnIsinWithAWrongCheckDigit) {
    // XX0000000010 is valid; the check digit 1 is wrong.
    const auto fault = payersFault(std::string{kPayersHeader} +
                                   "XX0000000011,,Payer A,,1 Main Street,IE,,,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileInvalidIsin);
}

TEST(ReferenceDataTest, FaultOfAConidWithLetters) {
    const auto fault =
        payersFault(std::string{kPayersHeader} + ",12A,Payer A,,1 Main Street,IE,,,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileInvalidConid);
}

TEST(ReferenceDataTest, FaultOfAnEmptyNameNamesTheColumn) {
    const auto fault =
        payersFault(std::string{kPayersHeader} + ",7,,,1 Main Street,IE,,,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileEmptyField);
    EXPECT_EQ(fault.mMessage.mParams, (std::vector<MessageParam>{textParam("field", "name")}));
}

TEST(ReferenceDataTest, FaultOfAnEmptyAddressNamesTheColumn) {
    const auto fault = payersFault(std::string{kPayersHeader} + ",7,Payer A,,,IE,,,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileEmptyField);
    EXPECT_EQ(fault.mMessage.mParams, (std::vector<MessageParam>{textParam("field", "address")}));
}

TEST(ReferenceDataTest, FaultOfACountryWithThreeLetters) {
    const auto fault =
        payersFault(std::string{kPayersHeader} + ",7,Payer A,,1 Main Street,IRL,,,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileInvalidCountry);
}

TEST(ReferenceDataTest, FaultOfAnInstrumentKindThatIsNotKnown) {
    const auto fault =
        payersFault(std::string{kPayersHeader} + ",7,Payer A,,1 Main Street,IE,stock,,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileInvalidInstrumentKind);
}

TEST(ReferenceDataTest, FaultOfAVerifiedDateThatIsNotADate) {
    // 2025-13-01 has no month 13.
    const auto fault = payersFault(std::string{kPayersHeader} +
                                   ",7,Payer A,,1 Main Street,IE,,2025-13-01,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileInvalidVerifiedOn);
}

TEST(ReferenceDataTest, FaultOfASourceThatIsNotKnown) {
    const auto fault =
        payersFault(std::string{kPayersHeader} + ",7,Payer A,,1 Main Street,IE,,,other,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileInvalidSource);
}

TEST(ReferenceDataTest, FaultOfAnIbEdavkiRowWithoutARevision) {
    const auto fault =
        payersFault(std::string{kPayersHeader} + ",7,Payer A,,1 Main Street,IE,,,ib-edavki,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileMissingSourceRevision);
}

TEST(ReferenceDataTest, FaultOfARowWithAMissingField) {
    // Row 2 has 9 fields, the header 10.
    const auto fault =
        payersFault(std::string{kPayersHeader} + ",7,Payer A,,1 Main Street,IE,,,manual\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileFieldCount);
    EXPECT_EQ(fault.mMessage.mParams,
              (std::vector<MessageParam>{numberParam("expected", 10), numberParam("found", 9)}));
}

TEST(ReferenceDataTest, FaultOfAFieldWithALineBreak) {
    // The address starts a quote on row 2 and closes it on the next line; the quotes are even.
    const auto fault = payersFault(std::string{kPayersHeader} +
                                   ",7,Payer A,,\"Level 20\n1 Main Street\",IE,,,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileLineBreak);
}

TEST(ReferenceDataTest, FaultOfAnUnclosedQuoteHasNoRow) {
    const auto fault =
        payersFault(std::string{kPayersHeader} + ",7,Payer A,,\"Level 20,IE,,,manual,\n");

    EXPECT_EQ(fault.mFile, "payers.csv");
    EXPECT_EQ(fault.mRow, std::nullopt);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileUnclosedQuote);
}

TEST(ReferenceDataTest, FaultOfAHeaderWithMissingColumns) {
    // The header lacks instrument_kind and verified_on.
    const auto fault =
        payersFault("isin,ibkr_conid,name,tax_number,address,country,source,source_revision\n");

    EXPECT_EQ(fault.mRow, std::nullopt);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileMissingColumns);
    EXPECT_EQ(fault.mMessage.mParams,
              (std::vector<MessageParam>{numberParam("count", 2),
                                         textParam("columns", "instrument_kind, verified_on")}));
}

TEST(ReferenceDataTest, FaultOfAHeaderWithARepeatedColumn) {
    const auto fault = payersFault(
        "isin,ibkr_conid,name,tax_number,address,country,instrument_kind,verified_on,source,"
        "source_revision,name\n");

    EXPECT_EQ(fault.mRow, std::nullopt);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileRepeatedColumn);
    EXPECT_EQ(fault.mMessage.mParams, (std::vector<MessageParam>{textParam("column", "name")}));
}

TEST(ReferenceDataTest, FaultOfAnEmptyReliefStatementNamesItsFile) {
    TemporaryFolder folder;
    const auto load = loadReferenceData(
        {folder.write("relief_statements.csv", std::string{kReliefHeader} + "BE,,manual,\n")});

    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "relief_statements.csv");
    EXPECT_EQ(load.mFault->mRow, 2U);
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileEmptyField);
    EXPECT_EQ(load.mFault->mMessage.mParams,
              (std::vector<MessageParam>{textParam("field", "statement")}));
}

TEST(ReferenceDataTest, FaultOfARepeatedBrokerAndCode) {
    TemporaryFolder folder;
    const auto load = loadReferenceData({folder.write(
        "broker_payers.csv",
        std::string{kBrokerHeader} + "IBKR,IB-X,Broker X,123,1 Main Street,GB,manual,\n"
                                     "IBKR,IB-X,Broker Y,456,2 Main Street,GB,manual,\n")});

    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "broker_payers.csv");
    EXPECT_EQ(load.mFault->mRow, 3U);
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileDuplicateBrokerCode);
}

TEST(ReferenceDataTest, AFaultHasTheEnglishTextAndNoValueOfTheFile) {
    const auto fault =
        payersFault(std::string{kPayersHeader} + ",7,Secret Name,,1 Main Street,IRL,,,manual,\n");

    EXPECT_EQ(fault.mMessage.mEnglish,
              "The country is not 2 uppercase letters. The reference data was not loaded.");
    EXPECT_EQ(fault.mMessage.mEnglish.find("Secret Name"), std::string::npos);
    EXPECT_EQ(fault.mMessage.mEnglish.find("IRL"), std::string::npos);
}

// A *LargeInputTest suite takes minutes under Valgrind. CI runs it on every push to main, and on a
// pull request only when a related file changes (`scripts/valgrind.sh --part reference`).
TEST(ReferenceDataLargeInputTest, ARowLongerThanTheReaderLimitMakesTheFileUnreadable) {
    // The CSV reader throws on a row longer than two 10 MB chunks. Row 2 has a name of
    // 21,000,000 characters.
    const std::string longName(21'000'000, 'x');
    const auto fault = payersFault(std::string{kPayersHeader} + "XX0000000010,," + longName +
                                   ",,1 Main Street,IE,,,manual,\n");

    EXPECT_EQ(fault.mFile, "payers.csv");
    EXPECT_EQ(fault.mRow, std::nullopt);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileUnreadable);
}

} // namespace
