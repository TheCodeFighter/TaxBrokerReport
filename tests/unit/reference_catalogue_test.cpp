#include "taxbroker/reference_catalogue.hpp"

#include "../support/large_allocation_hook.hpp"
#include "taxbroker/input_file.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
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
    "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
    "source_revision\n";
constexpr std::string_view kReliefHeader = "country,income,statement,source,source_revision\n";
constexpr std::string_view kBrokerHeader =
    "broker,code,name,tax_number,address,country,source,source_revision\n";

// One valid row each, for the tests that break only one of the other files.
constexpr std::string_view kOneRelief = "BE,dividend,Treaty text,manual,\n";
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

    EXPECT_EQ(catalogue.payers().size(), 282U); // 68 upstream ISINs + the ISINs found
    EXPECT_EQ(catalogue.allReliefStatements().size(), 17U);
    EXPECT_EQ(catalogue.brokerPayers().size(), 6U);
}

TEST(ReferenceCatalogueTest, TheEmbeddedReliefStatementsAreAllDividendTexts) {
    // No interest text is built in yet: the 17 upstream rows are dividend texts.
    const ReferenceCatalogue& catalogue = referenceCatalogue();

    EXPECT_EQ(catalogue.reliefStatements("US", TreatyIncome::Dividend).size(), 1U);
    EXPECT_TRUE(catalogue.reliefStatements("US", TreatyIncome::CashInterest).empty());
    EXPECT_TRUE(catalogue.reliefStatements("US", TreatyIncome::BondCoupon).empty());

    for (const auto& entry : catalogue.allReliefStatements())
    {
        EXPECT_EQ(entry.mIncome, TreatyIncome::Dividend);
    }
}

TEST(ReferenceCatalogueTest, EmbeddedPayersHaveNoKindNoVerificationDateAndTheUpstreamSource) {
    // Every row is upstream's except the Krka row: its tax number was corrected by hand, so its
    // source is `manual` (the ISIN still came from upstream).
    const ReferenceCatalogue& catalogue = referenceCatalogue();
    std::size_t manualRows = 0;

    for (const PayerEntry& entry : catalogue.payers())
    {
        EXPECT_FALSE(entry.mInstrumentKind.has_value());
        EXPECT_FALSE(entry.mVerifiedOn.has_value());
        EXPECT_EQ(entry.mSource.mFile, "payers.csv");

        if (entry.mIsin == "SI0031102120")
        {
            EXPECT_EQ(entry.mSource.mSource, "manual");
            ++manualRows;
        }
        else
        {
            EXPECT_EQ(entry.mSource.mSource, "ib-edavki");
        }
    }

    EXPECT_EQ(manualRows, 1U);
}

TEST(ReferenceCatalogueTest, EveryEmbeddedPayerHasAnIsinFromUpstreamOrWikidata) {
    const ReferenceCatalogue& catalogue = referenceCatalogue();
    std::size_t fromUpstream = 0;
    std::size_t fromWikidata = 0;

    for (const PayerEntry& entry : catalogue.payers())
    {
        EXPECT_EQ(entry.mIsin.size(), 12U);
        fromUpstream += entry.mIsinSource == "ib-edavki" ? 1 : 0;
        fromWikidata += entry.mIsinSource == "wikidata" ? 1 : 0;
    }

    EXPECT_EQ(fromUpstream, 68U);  // the payers that upstream gave an ISIN
    EXPECT_EQ(fromWikidata, 214U); // the ISINs found by the lookup
}

// --- Lookup ---

TEST(ReferenceCatalogueTest, FindsAPayerByExactIsin) {
    const auto catalogue =
        parse("XX0000000010,Payer A,TAX-A,1 Main Street,IE,etf,2026-01-15,issuer,manual,\n");

    const PayerLookup lookup = catalogue.findPayer(kIsinA);

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

TEST(ReferenceCatalogueTest, DoesNotMatchAnotherIsinWithTheSamePrefix) {
    const auto catalogue = parse("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n");

    const PayerLookup lookup = catalogue.findPayer(kIsinB);

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::NotFound);
    EXPECT_TRUE(lookup.mEntries.empty());
}

TEST(ReferenceCatalogueTest, ReportsAConflictForTwoRowsOfOneIsinWithAnotherName) {
    const auto catalogue = parse("XX0000000010,Payer X,,1 Main Street,IE,,,issuer,manual,\n"
                                 "XX0000000010,Payer Y,,2 Main Street,IE,,,wikidata,manual,\n");

    const PayerLookup lookup = catalogue.findPayer(kIsinA);

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Conflict);
    ASSERT_EQ(lookup.mEntries.size(), 2U);
    EXPECT_EQ(lookup.mEntries[0]->mSource.mRowNumber, 2U);
    EXPECT_EQ(lookup.mEntries[1]->mSource.mRowNumber, 3U);
}

TEST(ReferenceCatalogueTest, ReportsAConflictForTwoRowsOfOneIsinWithAnotherCountry) {
    const auto catalogue = parse("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n"
                                 "XX0000000010,Payer A,,1 Main Street,LU,,,issuer,manual,\n");

    const PayerLookup lookup = catalogue.findPayer(kIsinA);

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Conflict);
    EXPECT_EQ(lookup.mEntries.size(), 2U);
}

TEST(ReferenceCatalogueTest, TreatsTwoRowsWithTheSameFactsAsOne) {
    const auto catalogue =
        parse("XX0000000010,Payer A,TAX-A,1 Main Street,IE,,,ib-edavki,ib-edavki,abc123\n"
              "XX0000000010,Payer A,TAX-A,1 Main Street,IE,,2026-01-15,issuer,manual,\n");

    const PayerLookup lookup = catalogue.findPayer(kIsinA);

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Found);
    ASSERT_EQ(lookup.mEntries.size(), 1U);
    EXPECT_EQ(lookup.mEntries[0]->mSource.mRowNumber, 2U); // the first row in file order
}

TEST(ReferenceCatalogueTest, KeepsKindAndVerificationDateOfAManualRow) {
    const auto catalogue =
        parse("XX0000000036,Fund F,,3 Main Street,LU,fund,2026-01-15,issuer,manual,\n");

    const PayerLookup lookup = catalogue.findPayer("XX0000000036");

    ASSERT_EQ(lookup.mEntries.size(), 1U);
    EXPECT_EQ(lookup.mEntries[0]->mInstrumentKind, std::optional<std::string>{"fund"});
    EXPECT_EQ(lookup.mEntries[0]->mVerifiedOn, parseCalendarDate("2026-01-15"));
}

TEST(ReferenceCatalogueTest, FindsAnIsinWithALetterBeforeTheCheckDigit) {
    // XX00000000A -> 3333 00000000 10 (A counts as 10, two digits, which moves the doubling).
    // With the check digit last, the non-zero digits from the right are 1, 3 (doubled: 6), 3,
    // 3 (doubled: 6), 3: 1 + 6 + 3 + 6 + 3 = 19, so the check digit is 1 (sum 20).
    const auto catalogue = parse("XX00000000A1,Payer L,,4 Main Street,IE,,,issuer,manual,\n");

    const PayerLookup lookup = catalogue.findPayer("XX00000000A1");

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Found);
    ASSERT_EQ(lookup.mEntries.size(), 1U);
    EXPECT_EQ(lookup.mEntries[0]->mName, "Payer L");
}

TEST(ReferenceCatalogueTest, ReadsAQuotedAddressWithAComma) {
    const auto catalogue =
        parse("XX0000000010,Payer A,,\"Level 20, 1 Main Street\",IE,,,issuer,manual,\n");

    const PayerLookup lookup = catalogue.findPayer(kIsinA);

    ASSERT_EQ(lookup.mEntries.size(), 1U);
    EXPECT_EQ(lookup.mEntries[0]->mAddress, "Level 20, 1 Main Street");
}

TEST(ReferenceCatalogueTest, ListsTheConflictingRowsOfAnIsinInFileOrder) {
    // Rows 2 and 4 have the ISIN A with other names; row 3 has the ISIN B.
    const auto catalogue = parse("XX0000000010,Payer Y,,2 Main Street,IE,,,issuer,manual,\n"
                                 "XX0000000028,Payer B,,3 Main Street,IE,,,issuer,manual,\n"
                                 "XX0000000010,Payer X,,1 Main Street,IE,,,issuer,manual,\n");

    const PayerLookup lookup = catalogue.findPayer(kIsinA);

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Conflict);
    ASSERT_EQ(lookup.mEntries.size(), 2U);
    EXPECT_EQ(lookup.mEntries[0]->mName, "Payer Y"); // row 2, first in the file
    EXPECT_EQ(lookup.mEntries[1]->mName, "Payer X"); // row 4
    EXPECT_EQ(lookup.mEntries[1]->mSource.mRowNumber, 4U);
}

TEST(ReferenceCatalogueTest, KeepsTheIsinSourceOfTheRow) {
    const auto catalogue = parse("XX0000000010,Payer A,,1 Main Street,IE,,,wikidata,manual,\n");

    const PayerLookup lookup = catalogue.findPayer(kIsinA);

    ASSERT_EQ(lookup.mEntries.size(), 1U);
    EXPECT_EQ(lookup.mEntries[0]->mIsin, "XX0000000010");
    EXPECT_EQ(lookup.mEntries[0]->mIsinSource, "wikidata");
}

TEST(ReferenceCatalogueTest, IgnoresAnExtraConidColumnOfAnOlderFile) {
    // An older file still has the column ibkr_conid at the end; the reader ignores extra columns.
    const std::string payers =
        "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
        "source_revision,ibkr_conid\n"
        "XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,,12345\n";
    const auto catalogue =
        parseReferenceCatalogue(payers,
                                std::string{kReliefHeader} + std::string{kOneRelief},
                                std::string{kBrokerHeader} + std::string{kOneBroker});

    const PayerLookup lookup = catalogue.findPayer(kIsinA);

    EXPECT_EQ(lookup.mStatus, PayerLookupStatus::Found);
    EXPECT_EQ(catalogue.findPayer("12345").mStatus, PayerLookupStatus::NotFound); // not a key
}

TEST(ReferenceCatalogueTest, FindsABrokerPayerByBrokerAndCode) {
    const auto catalogue = parse("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                                 "BE,dividend,Treaty text,manual,\n",
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
    const auto catalogue = parse("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                                 "BE,dividend,Text one,manual,\n"
                                 "CA,dividend,Text two,manual,\n"
                                 "BE,dividend,Text three,manual,\n");

    const auto belgium = catalogue.reliefStatements("BE", TreatyIncome::Dividend);

    ASSERT_EQ(belgium.size(), 2U);
    EXPECT_EQ(belgium[0]->mStatement, "Text one");
    EXPECT_EQ(belgium[1]->mStatement, "Text three");
    EXPECT_EQ(catalogue.reliefStatements("CA", TreatyIncome::Dividend).size(), 1U);
    EXPECT_TRUE(catalogue.reliefStatements("DE", TreatyIncome::Dividend).empty());
}

TEST(ReferenceCatalogueTest, EachKindOfIncomeHasItsOwnReliefStatementsForACountry) {
    // US has one text for each kind; the lookup of a kind never returns another kind's text.
    const auto catalogue = parse("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                                 "US,dividend,Dividend text,manual,\n"
                                 "US,cash_interest,Cash interest text,manual,\n"
                                 "US,bond_coupon,Coupon text,manual,\n");

    const auto dividend = catalogue.reliefStatements("US", TreatyIncome::Dividend);
    const auto cashInterest = catalogue.reliefStatements("US", TreatyIncome::CashInterest);
    const auto bondCoupon = catalogue.reliefStatements("US", TreatyIncome::BondCoupon);

    ASSERT_EQ(dividend.size(), 1U);
    EXPECT_EQ(dividend[0]->mStatement, "Dividend text");
    EXPECT_EQ(dividend[0]->mIncome, TreatyIncome::Dividend);

    ASSERT_EQ(cashInterest.size(), 1U);
    EXPECT_EQ(cashInterest[0]->mStatement, "Cash interest text");
    EXPECT_EQ(cashInterest[0]->mIncome, TreatyIncome::CashInterest);

    ASSERT_EQ(bondCoupon.size(), 1U);
    EXPECT_EQ(bondCoupon[0]->mStatement, "Coupon text");
    EXPECT_EQ(bondCoupon[0]->mIncome, TreatyIncome::BondCoupon);
}

TEST(ReferenceCatalogueTest, ACountryWithOnlyOneKindOfReliefStatementHasNoneForTheOthers) {
    const auto catalogue = parse("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                                 "US,bond_coupon,Coupon text,manual,\n");

    EXPECT_EQ(catalogue.reliefStatements("US", TreatyIncome::BondCoupon).size(), 1U);
    EXPECT_TRUE(catalogue.reliefStatements("US", TreatyIncome::Dividend).empty());
    EXPECT_TRUE(catalogue.reliefStatements("US", TreatyIncome::CashInterest).empty());
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

TEST(ReferenceCatalogueTest, RejectsARowWithoutIsin) {
    // Row 2 has an empty isin.
    expectFault(",Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                "payers.csv row 2: the isin is empty");
}

TEST(ReferenceCatalogueTest, RejectsARowWithoutIsinSource) {
    // Row 2 has an empty isin_source.
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,,,manual,\n",
                "payers.csv row 2: the isin_source is empty");
}

TEST(ReferenceCatalogueTest, RejectsAnUnknownIsinSource) {
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,,openfigi,manual,\n",
                "payers.csv row 2: the ISIN source is not ib-edavki, wikidata, issuer, exchange or "
                "gleif");
}

TEST(ReferenceCatalogueTest, AcceptsEveryKnownIsinSource) {
    const auto catalogue = parse("XX0000000010,Payer A,,1 Main Street,IE,,,ib-edavki,manual,\n"
                                 "XX0000000028,Payer B,,2 Main Street,IE,,,wikidata,manual,\n"
                                 "XX0000000036,Payer C,,3 Main Street,IE,,,issuer,manual,\n"
                                 "XX0000000044,Payer D,,4 Main Street,IE,,,exchange,manual,\n"
                                 "XX0000000051,Payer E,,5 Main Street,IE,,,gleif,manual,\n");

    ASSERT_EQ(catalogue.payers().size(), 5U);
    EXPECT_EQ(catalogue.payers()[0].mIsinSource, "ib-edavki");
    EXPECT_EQ(catalogue.payers()[1].mIsinSource, "wikidata");
    EXPECT_EQ(catalogue.payers()[2].mIsinSource, "issuer");
    EXPECT_EQ(catalogue.payers()[3].mIsinSource, "exchange");
    EXPECT_EQ(catalogue.payers()[4].mIsinSource, "gleif");
}

TEST(ReferenceCatalogueTest, RejectsAnIsinWithAWrongCheckDigit) {
    // XX0000000011: the check digit of XX000000001 is 0.
    expectFault("XX0000000011,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                "payers.csv row 2: the ISIN is not valid");
}

TEST(ReferenceCatalogueTest, RejectsAnIsinWithALetterAndAWrongCheckDigit) {
    // XX00000000A0: the check digit of XX00000000A is 1.
    expectFault("XX00000000A0,Payer L,,4 Main Street,IE,,,issuer,manual,\n",
                "payers.csv row 2: the ISIN is not valid");
}

TEST(ReferenceCatalogueTest, RejectsAnIsinOfWrongLength) {
    // 11 characters.
    expectFault("XX000000001,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                "payers.csv row 2: the ISIN is not valid");
}

TEST(ReferenceCatalogueTest, RejectsAnIsinWithALowercaseLetter) {
    expectFault("xx0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                "payers.csv row 2: the ISIN is not valid");
}

TEST(ReferenceCatalogueTest, RejectsAThreeLetterCountry) {
    expectFault("XX0000000010,Payer A,,1 Main Street,IRL,,,issuer,manual,\n",
                "payers.csv row 2: the country is not 2 uppercase letters");
}

TEST(ReferenceCatalogueTest, RejectsAnEmptyName) {
    expectFault("XX0000000010,,,1 Main Street,IE,,,issuer,manual,\n",
                "payers.csv row 2: the name is empty");
}

TEST(ReferenceCatalogueTest, RejectsAnEmptyAddress) {
    expectFault("XX0000000010,Payer A,,,IE,,,issuer,manual,\n",
                "payers.csv row 2: the address is empty");
}

TEST(ReferenceCatalogueTest, RejectsAnUnknownSource) {
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,other,\n",
                "payers.csv row 2: the source is not ib-edavki or manual");
}

TEST(ReferenceCatalogueTest, RejectsAnUpstreamRowWithoutRevision) {
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,,ib-edavki,ib-edavki,\n",
                "payers.csv row 2: an ib-edavki row has no source revision");
}

TEST(ReferenceCatalogueTest, RejectsAnInvalidVerificationDate) {
    // Month 13.
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,2026-13-01,issuer,manual,\n",
                "payers.csv row 2: verified_on is not a date as YYYY-MM-DD");
}

TEST(ReferenceCatalogueTest, RejectsAnUnknownInstrumentKind) {
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,crypto,,issuer,manual,\n",
                "payers.csv row 2: the instrument kind is not share, etf, fund or bond");
}

TEST(ReferenceCatalogueTest, ReportsTheLogicalRowOfTheFault) {
    // Row 2 is valid; row 3 has no name.
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\nXX0000000028,,,2 Main "
                "Street,IE,,,issuer,manual,\n",
                "payers.csv row 3: the name is empty");
}

TEST(ReferenceCatalogueTest, RejectsARowWithTooFewFields) {
    // 9 fields: source_revision is missing.
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual\n",
                "payers.csv row 2: the row has 9 fields, the header 10");
}

TEST(ReferenceCatalogueTest, RejectsAPayersFileWithoutAColumn) {
    // The header lacks instrument_kind, verified_on and isin_source.
    const std::string payers = "isin,name,tax_number,address,country,source,source_revision\n";

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
                  "payers.csv: the header lacks the columns instrument_kind, verified_on, "
                  "isin_source");
    }
}

TEST(ReferenceCatalogueTest, RejectsAnIsinWithASymbol) {
    // 12 characters, but the 7th is a hyphen.
    expectFault("XX0000-00010,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                "payers.csv row 2: the ISIN is not valid");
}

TEST(ReferenceCatalogueTest, RejectsAPayersFileWithARepeatedColumn) {
    // The header has the column name twice.
    const std::string payers =
        "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
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
    expectFault("XX0000000010,Payer A,,\"Level 20\n1 Main Street\",IE,,,issuer,manual,\n",
                "payers.csv row 2: a field holds a line break");
}

TEST(ReferenceCatalogueTest, RejectsAPayersFileWithAnUnclosedQuote) {
    // The quote before Level never closes.
    expectFault("XX0000000010,Payer A,,\"Level 20,IE,,,issuer,manual,\n",
                "payers.csv: a quote is not closed");
}

TEST(ReferenceCatalogueTest, RejectsAnEmptyReliefStatement) {
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                "relief_statements.csv row 2: the statement is empty",
                "BE,dividend,,manual,\n");
}

TEST(ReferenceCatalogueTest, RejectsAReliefStatementWithAnUnknownIncome) {
    // "interest" is not one of dividend, cash_interest and bond_coupon.
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                "relief_statements.csv row 2: the income is not dividend, cash_interest or "
                "bond_coupon",
                "BE,interest,Treaty text,manual,\n");
}

TEST(ReferenceCatalogueTest, RejectsAReliefStatementWithAnEmptyIncome) {
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                "relief_statements.csv row 2: the income is not dividend, cash_interest or "
                "bond_coupon",
                "BE,,Treaty text,manual,\n");
}

TEST(ReferenceCatalogueTest, RejectsAReliefStatementWithAWrongCountry) {
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                "relief_statements.csv row 2: the country is not 2 uppercase letters",
                "Belgium,dividend,Treaty text,manual,\n");
}

TEST(ReferenceCatalogueTest, RejectsARepeatedBrokerPayer) {
    // Rows 2 and 3 have the broker IBKR and the code IB-X.
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                "broker_payers.csv row 3: the broker and code are already in the file",
                "BE,dividend,Treaty text,manual,\n",
                "IBKR,IB-X,Broker X,123,1 Main Street,GB,manual,\n"
                "IBKR,IB-X,Broker Y,456,2 Main Street,GB,manual,\n");
}

TEST(ReferenceCatalogueTest, RejectsABrokerPayerWithoutACode) {
    expectFault("XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n",
                "broker_payers.csv row 2: the code is empty",
                "BE,dividend,Treaty text,manual,\n",
                "IBKR,,Broker X,123,1 Main Street,GB,manual,\n");
}

TEST(ReferenceCatalogueTest, DoesNotPutAValueOfTheFileInAFaultMessage) {
    try
    {
        static_cast<void>(parse("XX0000000010,Secret Name,,1 Main Street,IRL,,,issuer,manual,\n"));
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
    std::string{kPayersHeader} + "XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual,\n";

/// Loads one supplied payers file with this text and returns the fault, which must be set.
ReferenceFileFault payersFault(std::string_view aText) {
    TemporaryFolder folder;
    const auto load = loadReferenceData({folder.write("payers.csv", aText)});

    EXPECT_FALSE(load.mCatalogue.has_value());
    EXPECT_TRUE(load.mFault.has_value());

    return load.mFault.value_or(ReferenceFileFault{});
}

TEST(ReferenceDataTest, NoFilesGiveTheBuiltInData) {
    const auto load = loadReferenceData(std::vector<std::filesystem::path>{});

    ASSERT_TRUE(load.mCatalogue.has_value());
    EXPECT_FALSE(load.mFault.has_value());
    EXPECT_EQ(load.mCatalogue->payers().size(), 282U);
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
    ASSERT_EQ(load.mCatalogue->payers().size(), 1U); // the 282 built-in payers are gone

    const auto found = load.mCatalogue->findPayer(kIsinA);

    ASSERT_EQ(found.mStatus, PayerLookupStatus::Found);
    EXPECT_EQ(found.mEntries.front()->mName, "Payer A");
    EXPECT_EQ(found.mEntries.front()->mSource.mOrigin, CatalogueOrigin::Supplied);
    EXPECT_EQ(found.mEntries.front()->mSource.mFile, "payers.csv");
    EXPECT_EQ(found.mEntries.front()->mSource.mRowNumber, 2U); // the header is row 1

    // An ISIN of the built-in file is not found in the supplied one.
    const auto& builtInPayers = referenceCatalogue().payers();

    ASSERT_FALSE(builtInPayers.empty());

    const auto missing = load.mCatalogue->findPayer(builtInPayers.front().mIsin);

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
                      std::string{kReliefHeader} + "BE,dividend,Treaty text,manual,\n"),
         folder.write("broker_payers.csv", std::string{kBrokerHeader} + std::string{kOneBroker})});

    ASSERT_TRUE(load.mCatalogue.has_value());
    EXPECT_EQ(load.mOrigins[0], CatalogueOrigin::BuiltIn);
    EXPECT_EQ(load.mOrigins[1], CatalogueOrigin::Supplied);
    EXPECT_EQ(load.mOrigins[2], CatalogueOrigin::Supplied);

    EXPECT_EQ(load.mCatalogue->payers().size(), 282U);

    ASSERT_EQ(load.mCatalogue->allReliefStatements().size(), 1U);
    EXPECT_EQ(load.mCatalogue->allReliefStatements().front().mStatement, "Treaty text");

    ASSERT_EQ(load.mCatalogue->brokerPayers().size(), 1U);
    EXPECT_EQ(load.mCatalogue->brokerPayers().front().mCode, "IB-X");
}

TEST(ReferenceDataTest, TheBuiltInListsAreSharedByEveryLoadAndByTheEmbeddedCatalogue) {
    const auto first = loadReferenceData(std::vector<std::filesystem::path>{});
    const auto second = loadReferenceData(std::vector<std::filesystem::path>{});

    ASSERT_TRUE(first.mCatalogue.has_value());
    ASSERT_TRUE(second.mCatalogue.has_value());

    // The same list objects, not copies of them.
    EXPECT_EQ(&first.mCatalogue->payers(), &second.mCatalogue->payers());
    EXPECT_EQ(&first.mCatalogue->payers(), &referenceCatalogue().payers());
    EXPECT_EQ(&first.mCatalogue->allReliefStatements(),
              &referenceCatalogue().allReliefStatements());
    EXPECT_EQ(&first.mCatalogue->brokerPayers(), &referenceCatalogue().brokerPayers());
}

TEST(ReferenceDataTest, ASuppliedFileSharesTheOtherTwoBuiltInLists) {
    TemporaryFolder folder;
    const auto load = loadReferenceData({folder.write("payers.csv", kSuppliedPayers)});

    ASSERT_TRUE(load.mCatalogue.has_value());

    // The supplied payers are a new list; the other two lists are the shared built-in ones.
    EXPECT_NE(&load.mCatalogue->payers(), &referenceCatalogue().payers());
    EXPECT_EQ(&load.mCatalogue->allReliefStatements(), &referenceCatalogue().allReliefStatements());
    EXPECT_EQ(&load.mCatalogue->brokerPayers(), &referenceCatalogue().brokerPayers());
}

TEST(ReferenceDataTest, ACopyOfACatalogueSharesItsLists) {
    const ReferenceCatalogue copy = referenceCatalogue();

    EXPECT_EQ(&copy.payers(), &referenceCatalogue().payers());
    EXPECT_EQ(copy.payers().size(), 282U);
}

TEST(ReferenceDataTest, ACatalogueWithoutAListIsABuildError) {
    EXPECT_THROW(ReferenceCatalogue(
                     nullptr,
                     std::make_shared<const std::vector<ReliefStatementEntry>>(),
                     std::make_shared<const BrokerPayerList>(std::vector<BrokerPayerEntry>{})),
                 std::logic_error);
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

TEST(ReferenceDataTest, FaultOfARowWithoutIsinNamesTheColumn) {
    const auto fault =
        payersFault(std::string{kPayersHeader} + ",Payer A,,1 Main Street,IE,,,issuer,manual,\n");

    EXPECT_EQ(fault.mFile, "payers.csv");
    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileEmptyField);
    EXPECT_EQ(fault.mMessage.mParams, (std::vector<MessageParam>{textParam("field", "isin")}));
}

TEST(ReferenceDataTest, FaultOfAnIsinSourceThatIsNotKnown) {
    const auto fault = payersFault(std::string{kPayersHeader} +
                                   "XX0000000010,Payer A,,1 Main Street,IE,,,openfigi,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileInvalidIsinSource);
    EXPECT_TRUE(fault.mMessage.mParams.empty());
}

TEST(ReferenceDataTest, FaultOfAnIsinWithAWrongCheckDigit) {
    // XX0000000010 is valid; the check digit 1 is wrong.
    const auto fault = payersFault(std::string{kPayersHeader} +
                                   "XX0000000011,Payer A,,1 Main Street,IE,,,issuer,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileInvalidIsin);
}

TEST(ReferenceDataTest, FaultOfAnEmptyNameNamesTheColumn) {
    const auto fault = payersFault(std::string{kPayersHeader} +
                                   "XX0000000010,,,1 Main Street,IE,,,issuer,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileEmptyField);
    EXPECT_EQ(fault.mMessage.mParams, (std::vector<MessageParam>{textParam("field", "name")}));
}

TEST(ReferenceDataTest, FaultOfAnEmptyAddressNamesTheColumn) {
    const auto fault =
        payersFault(std::string{kPayersHeader} + "XX0000000010,Payer A,,,IE,,,issuer,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileEmptyField);
    EXPECT_EQ(fault.mMessage.mParams, (std::vector<MessageParam>{textParam("field", "address")}));
}

TEST(ReferenceDataTest, FaultOfACountryWithThreeLetters) {
    const auto fault = payersFault(std::string{kPayersHeader} +
                                   "XX0000000010,Payer A,,1 Main Street,IRL,,,issuer,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileInvalidCountry);
}

TEST(ReferenceDataTest, FaultOfAnInstrumentKindThatIsNotKnown) {
    const auto fault =
        payersFault(std::string{kPayersHeader} +
                    "XX0000000010,Payer A,,1 Main Street,IE,stock,,issuer,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileInvalidInstrumentKind);
}

TEST(ReferenceDataTest, FaultOfAVerifiedDateThatIsNotADate) {
    // 2025-13-01 has no month 13.
    const auto fault =
        payersFault(std::string{kPayersHeader} +
                    "XX0000000010,Payer A,,1 Main Street,IE,,2025-13-01,issuer,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileInvalidVerifiedOn);
}

TEST(ReferenceDataTest, FaultOfASourceThatIsNotKnown) {
    const auto fault = payersFault(std::string{kPayersHeader} +
                                   "XX0000000010,Payer A,,1 Main Street,IE,,,issuer,other,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileInvalidSource);
}

TEST(ReferenceDataTest, FaultOfAnIbEdavkiRowWithoutARevision) {
    const auto fault =
        payersFault(std::string{kPayersHeader} +
                    "XX0000000010,Payer A,,1 Main Street,IE,,,ib-edavki,ib-edavki,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileMissingSourceRevision);
}

TEST(ReferenceDataTest, FaultOfARowWithAMissingField) {
    // Row 2 has 9 fields, the header 10.
    const auto fault = payersFault(std::string{kPayersHeader} +
                                   "XX0000000010,Payer A,,1 Main Street,IE,,,issuer,manual\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileFieldCount);
    EXPECT_EQ(fault.mMessage.mParams,
              (std::vector<MessageParam>{numberParam("expected", 10), numberParam("found", 9)}));
}

TEST(ReferenceDataTest, FaultOfAFieldWithALineBreak) {
    // The address starts a quote on row 2 and closes it on the next line; the quotes are even.
    const auto fault =
        payersFault(std::string{kPayersHeader} +
                    "XX0000000010,Payer A,,\"Level 20\n1 Main Street\",IE,,,issuer,manual,\n");

    EXPECT_EQ(fault.mRow, 2U);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileLineBreak);
}

TEST(ReferenceDataTest, FaultOfAnUnclosedQuoteHasNoRow) {
    const auto fault = payersFault(std::string{kPayersHeader} +
                                   "XX0000000010,Payer A,,\"Level 20,IE,,,issuer,manual,\n");

    EXPECT_EQ(fault.mFile, "payers.csv");
    EXPECT_EQ(fault.mRow, std::nullopt);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileUnclosedQuote);
}

TEST(ReferenceDataTest, FaultOfAHeaderWithMissingColumns) {
    // The header lacks instrument_kind, verified_on and isin_source.
    const auto fault = payersFault("isin,name,tax_number,address,country,source,source_revision\n");

    EXPECT_EQ(fault.mRow, std::nullopt);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileMissingColumns);
    EXPECT_EQ(fault.mMessage.mParams,
              (std::vector<MessageParam>{
                  numberParam("count", 3),
                  textParam("columns", "instrument_kind, verified_on, isin_source")}));
}

TEST(ReferenceDataTest, FaultOfAHeaderWithARepeatedColumn) {
    const auto fault = payersFault(
        "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
        "source_revision,name\n");

    EXPECT_EQ(fault.mRow, std::nullopt);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileRepeatedColumn);
    EXPECT_EQ(fault.mMessage.mParams, (std::vector<MessageParam>{textParam("column", "name")}));
}

TEST(ReferenceDataTest, FaultOfAnEmptyReliefStatementNamesItsFile) {
    TemporaryFolder folder;
    const auto load =
        loadReferenceData({folder.write("relief_statements.csv",
                                        std::string{kReliefHeader} + "BE,dividend,,manual,\n")});

    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "relief_statements.csv");
    EXPECT_EQ(load.mFault->mRow, 2U);
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileEmptyField);
    EXPECT_EQ(load.mFault->mMessage.mParams,
              (std::vector<MessageParam>{textParam("field", "statement")}));
}

TEST(ReferenceDataTest, FaultOfAnUnknownTreatyIncomeNamesItsFile) {
    TemporaryFolder folder;
    const auto load = loadReferenceData(
        {folder.write("relief_statements.csv",
                      std::string{kReliefHeader} + "BE,interest,Treaty text,manual,\n")});

    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "relief_statements.csv");
    EXPECT_EQ(load.mFault->mRow, 2U);
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileInvalidTreatyIncome);
}

TEST(ReferenceDataTest, FaultOfAReliefFileWithoutTheIncomeColumn) {
    // The header lacks income, which dividend texts of an older file do not have.
    TemporaryFolder folder;
    const auto load = loadReferenceData(
        {folder.write("relief_statements.csv",
                      "country,statement,source,source_revision\nBE,Treaty text,manual,\n")});

    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "relief_statements.csv");
    EXPECT_EQ(load.mFault->mRow, std::nullopt);
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileMissingColumns);
    EXPECT_EQ(load.mFault->mMessage.mParams,
              (std::vector<MessageParam>{numberParam("count", 1), textParam("columns", "income")}));
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
        payersFault(std::string{kPayersHeader} +
                    "XX0000000010,Secret Name,,1 Main Street,IRL,,,issuer,manual,\n");

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
    const auto fault = payersFault(std::string{kPayersHeader} + "XX0000000010," + longName +
                                   ",,1 Main Street,IE,,,issuer,manual,\n");

    EXPECT_EQ(fault.mFile, "payers.csv");
    EXPECT_EQ(fault.mRow, std::nullopt);
    EXPECT_EQ(fault.mMessage.mKey, MessageKey::ReferenceFileUnreadable);
}

TEST(ReferenceCatalogueTest, BrokerPayersOfABrokerAreItsRowsInFileOrder) {
    const ReferenceCatalogue catalogue = parse("",
                                               "",
                                               "IBKR,IB-A,Broker A,111,1 Main Street,GB,manual,\n"
                                               "TR,TR-A,Broker T,222,2 Main Street,DE,manual,\n"
                                               "IBKR,IB-B,Broker B,333,3 Main Street,IE,manual,\n");

    const auto interactive = catalogue.brokerPayersOf(Broker::InteractiveBrokers);

    ASSERT_EQ(interactive.size(), 2U);
    EXPECT_EQ(interactive.at(0)->mCode, "IB-A");
    EXPECT_EQ(interactive.at(1)->mCode, "IB-B");

    const auto tradeRepublic = catalogue.brokerPayersOf(Broker::TradeRepublic);

    ASSERT_EQ(tradeRepublic.size(), 1U);
    EXPECT_EQ(tradeRepublic.at(0)->mName, "Broker T");
    EXPECT_TRUE(catalogue.brokerPayersOf(Broker::Unknown).empty());
}

TEST(ReferenceCatalogueTest, TheEmbeddedFileHasOneTradeRepublicCompany) {
    const auto tradeRepublic = referenceCatalogue().brokerPayersOf(Broker::TradeRepublic);

    ASSERT_EQ(tradeRepublic.size(), 1U);
    EXPECT_EQ(tradeRepublic.at(0)->mName, "Trade Republic Bank GmbH");
    EXPECT_EQ(tradeRepublic.at(0)->mTaxNumber, std::optional<std::string>{"DE307510626"});
    EXPECT_EQ(tradeRepublic.at(0)->mAddress, "Brunnenstr. 19-21, 10119 Berlin");
    EXPECT_EQ(tradeRepublic.at(0)->mCountry, "DE");
}

TEST(ReferenceDataTest, ASuppliedPayersFileInMemoryReplacesTheBuiltInFile) {
    const std::vector<InputFile> files{
        InputFile{.mName = "payers.csv", .mContent = kSuppliedPayers}};
    const auto load = loadReferenceData(files);

    ASSERT_TRUE(load.mCatalogue.has_value());
    EXPECT_EQ(load.mOrigins[0], CatalogueOrigin::Supplied);
    EXPECT_EQ(load.mOrigins[1], CatalogueOrigin::BuiltIn);
    EXPECT_EQ(load.mOrigins[2], CatalogueOrigin::BuiltIn);
    ASSERT_EQ(load.mCatalogue->payers().size(), 1U);

    const auto found = load.mCatalogue->findPayer(kIsinA);

    ASSERT_EQ(found.mStatus, PayerLookupStatus::Found);
    EXPECT_EQ(found.mEntries.front()->mName, "Payer A");
    EXPECT_EQ(found.mEntries.front()->mSource.mFile, "payers.csv");
}

TEST(ReferenceDataTest, AMemoryFileNameWithAFolderPartCountsByItsLastPart) {
    const std::vector<InputFile> files{
        InputFile{.mName = "upload/payers.csv", .mContent = kSuppliedPayers}};
    const auto load = loadReferenceData(files);

    ASSERT_TRUE(load.mCatalogue.has_value());
    EXPECT_EQ(load.mOrigins[0], CatalogueOrigin::Supplied);
}

TEST(ReferenceDataTest, AMemoryFileNameThatIsNotOneOfTheThreeIsAFault) {
    const std::vector<InputFile> files{
        InputFile{.mName = "other.csv", .mContent = kSuppliedPayers}};
    const auto load = loadReferenceData(files);

    EXPECT_FALSE(load.mCatalogue.has_value());
    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "other.csv");
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileUnknownName);
}

TEST(ReferenceDataTest, ANameThatIsNotAFileNameIsAnUnknownNameFault) {
    const std::vector<InputFile> files{InputFile{.mName = "", .mContent = kSuppliedPayers}};
    const auto load = loadReferenceData(files);

    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "");
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileUnknownName);
    EXPECT_EQ(load.mFault->mMessage.mParams,
              (std::vector<MessageParam>{textParam("name", "<no file name>")}));
}

TEST(ReferenceDataTest, ANameThatEndsInAFolderGivesAnUnknownNameFaultWithoutTheFolder) {
    const std::vector<InputFile> files{
        InputFile{.mName = "/home/user/private/", .mContent = kSuppliedPayers}};
    const auto load = loadReferenceData(files);

    EXPECT_FALSE(load.mCatalogue.has_value());
    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "");
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::ReferenceFileUnknownName);
    EXPECT_EQ(load.mFault->mMessage.mParams,
              (std::vector<MessageParam>{textParam("name", "<no file name>")}));
}

TEST(ReferenceDataTest, APayersFileInMemoryOverTheSizeLimitIsAFaultWithItsOwnMessage) {
    const std::vector<InputFile> files{
        InputFile{.mName = "payers.csv", .mContent = kSuppliedPayers}};

    const auto load = loadReferenceData(files, kSuppliedPayers.size() - 1);

    EXPECT_FALSE(load.mCatalogue.has_value());
    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "payers.csv");
    EXPECT_EQ(load.mFault->mRow, std::nullopt);
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::InputFileTooLarge);
    EXPECT_EQ(load.mFault->mMessage.mParams,
              (std::vector<MessageParam>{numberParam("limitMb", 0)}));
}

TEST(ReferenceDataTest, APayersFileOnDiskOverTheSizeLimitIsNotRead) {
    TemporaryFolder folder;
    const auto path = folder.write("payers.csv", kSuppliedPayers);

    const auto load = loadReferenceData({path}, kSuppliedPayers.size() - 1);

    EXPECT_FALSE(load.mCatalogue.has_value());
    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "payers.csv");
    EXPECT_EQ(load.mFault->mRow, std::nullopt);
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::InputFileTooLarge);
    // The test limit is below 1 MB, so it is 0 whole megabytes.
    EXPECT_EQ(load.mFault->mMessage.mParams,
              (std::vector<MessageParam>{numberParam("limitMb", 0)}));
}

TEST(ReferenceDataTest, RunningOutOfMemoryIsAFaultWithItsOwnMessage) {
    // A file of the size of a large allocation, so its text takes one.
    const std::vector<InputFile> files{
        InputFile{.mName = "payers.csv",
                  .mContent = kSuppliedPayers + std::string(test::kLargeAllocationBytes, '\n')}};

    ReferenceDataLoad load;

    {
        const test::ScopedAllocationFailure failure;

        load = loadReferenceData(files);
    }

    EXPECT_FALSE(load.mCatalogue.has_value());
    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "payers.csv");
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::InputFileOutOfMemory);
    EXPECT_EQ(load.mFault->mMessage.mEnglish,
              "There was not enough memory to read the file. Close other programs and try again.");
}

TEST(ReferenceDataTest, RunningOutOfMemoryWhileParsingAFileNamesThatFile) {
    // A small payers file and a large relief-statements file. The large allocations are, in
    // order: the copy of the relief text for the loader (1st), the buffer of the CSV reader of
    // the payers file (2nd), then the allocations for the relief file (3rd on). Failing from the
    // 3rd means both texts were read and the parsing of the second file failed, so the fault
    // names that file and not the first one.
    const std::vector<InputFile> files{
        InputFile{.mName = "payers.csv", .mContent = kSuppliedPayers},
        InputFile{.mName = "relief_statements.csv",
                  .mContent = "country,income,statement,source,source_revision\n" +
                              std::string(test::kLargeAllocationBytes, '\n')}};

    ReferenceDataLoad load;

    {
        const test::ScopedAllocationFailure failure{3};

        load = loadReferenceData(files);
    }

    EXPECT_FALSE(load.mCatalogue.has_value());
    ASSERT_TRUE(load.mFault.has_value());
    EXPECT_EQ(load.mFault->mFile, "relief_statements.csv");
    EXPECT_EQ(load.mFault->mRow, std::nullopt);
    EXPECT_EQ(load.mFault->mMessage.mKey, MessageKey::InputFileOutOfMemory);
}

} // namespace
