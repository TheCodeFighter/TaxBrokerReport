#include "taxbroker/exact_arithmetic.hpp"

#include <gtest/gtest.h>
#include <pugixml.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// These tests check the synthetic IBKR fixtures in tests/test_data/ibkr against what the export
// contract (docs/architecture.md, "Interactive Brokers export contract") fixes. They do not call
// a parser. The expected result of each fixture is in tests/test_data/ibkr/README.md.

namespace {
using taxbroker::ExactRational;
using taxbroker::WideInteger;

std::filesystem::path fixtureFolder() {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / "ibkr";
}

std::filesystem::path ecbFolder() {
    return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "resources" /
           "ecb";
}

/// Reads a whole file byte for byte; a file that cannot be opened fails the test.
std::string readFile(const std::filesystem::path& aPath) {
    std::ifstream input{aPath, std::ios::binary};
    EXPECT_TRUE(input.is_open()) << aPath.filename().string();

    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/// Loads one XML fixture; a file that is not well-formed XML fails the test.
pugi::xml_document loadFixture(const std::string& aName) {
    pugi::xml_document document;
    const auto result = document.load_file((fixtureFolder() / aName).string().c_str());

    EXPECT_EQ(result.status, pugi::status_ok) << aName << ": " << result.description();

    return document;
}

/// Reads a plain decimal such as "-0.35" or "0.1234567890123456789012345678901" as an exact
/// fraction, so a 31-decimal quantity loses nothing.
ExactRational exactDecimal(std::string_view aText) {
    const bool negative = !aText.empty() && aText.front() == '-';

    if (negative)
    {
        aText.remove_prefix(1);
    }

    const auto dot = aText.find('.');
    std::string digits{aText.substr(0, dot)};
    std::size_t decimals = 0;

    if (dot != std::string_view::npos)
    {
        digits += aText.substr(dot + 1);
        decimals = aText.size() - dot - 1;
    }

    // A leading zero would make the string an octal number.
    const auto firstDigit = digits.find_first_not_of('0');
    digits = firstDigit == std::string::npos ? "0" : digits.substr(firstDigit);

    WideInteger numerator{digits};

    if (negative)
    {
        numerator = -numerator;
    }

    WideInteger denominator{1};

    for (std::size_t i = 0; i < decimals; ++i)
    {
        denominator *= 10;
    }

    return std::get<ExactRational>(ExactRational::create(numerator, denominator));
}

/// The `FlexStatement` elements of a loaded fixture.
std::vector<pugi::xml_node> statementsOf(const pugi::xml_document& aDocument) {
    const auto statements =
        aDocument.child("FlexQueryResponse").child("FlexStatements").children("FlexStatement");

    return std::vector<pugi::xml_node>{statements.begin(), statements.end()};
}

/// Checks every EXECUTION `Trade` of a fixture: proceeds = -quantity x price x multiplier exactly,
/// the sign agrees with buySell, and the commission is not positive. Returns how many were checked.
std::size_t checkExecutions(const std::string& aName) {
    const auto document = loadFixture(aName);
    std::size_t checked = 0;

    for (const auto& statement : statementsOf(document))
    {
        for (const auto trade : statement.child("Trades").children("Trade"))
        {
            if (std::string_view{trade.attribute("levelOfDetail").value()} != "EXECUTION")
            {
                continue;
            }

            const std::string id = aName + " " + trade.attribute("transactionID").value();
            const auto quantity = exactDecimal(trade.attribute("quantity").value());
            const auto price = exactDecimal(trade.attribute("tradePrice").value());
            const auto multiplier = exactDecimal(trade.attribute("multiplier").value());
            const auto proceeds = exactDecimal(trade.attribute("proceeds").value());

            const auto notional = std::get<ExactRational>(taxbroker::multiplyExact(
                std::get<ExactRational>(taxbroker::multiplyExact(quantity, price)),
                multiplier));
            const auto sum = std::get<ExactRational>(taxbroker::addExact(proceeds, notional));
            EXPECT_EQ(taxbroker::compareExact(sum, ExactRational{WideInteger{0}}), 0) << id;

            const std::string_view side = trade.attribute("buySell").value();
            const int quantitySign =
                taxbroker::compareExact(quantity, ExactRational{WideInteger{0}});
            EXPECT_EQ(side == "BUY" ? 1 : -1, quantitySign) << id;

            const auto commission = exactDecimal(trade.attribute("ibCommission").value());
            EXPECT_LE(taxbroker::compareExact(commission, ExactRational{WideInteger{0}}), 0) << id;

            ++checked;
        }
    }

    return checked;
}

/// The check digit rule of an ISIN: letters are two digits (A = 10), then the Luhn sum of the
/// whole code, doubling every second digit from the right of the check digit, is a multiple of 10.
bool hasValidIsinCheckDigit(const std::string& aIsin) {
    if (aIsin.size() != 12)
    {
        return false;
    }

    std::string digits;

    for (const char letter : aIsin)
    {
        if (letter >= '0' && letter <= '9')
        {
            digits += letter;
        }
        else
        {
            digits += std::to_string(letter - 'A' + 10);
        }
    }

    int total = 0;

    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        int digit = digits[digits.size() - 1 - i] - '0';

        if (i % 2 == 1)
        {
            digit *= 2;
            digit = digit / 10 + digit % 10;
        }

        total += digit;
    }

    return total % 10 == 0;
}

/// Collects the `isin` attribute of every element below aNode.
void collectIsins(const pugi::xml_node& aNode, std::set<std::string>& aIsins) {
    const std::string isin = aNode.attribute("isin").value();

    if (!isin.empty())
    {
        aIsins.insert(isin);
    }

    for (const auto child : aNode.children())
    {
        collectIsins(child, aIsins);
    }
}

/// Collects the date of every tax-relevant date attribute below aNode: tradeDate, the date part of
/// dateTime, settleDate, reportDate and the date of a transfer, as yyyyMMdd.
void collectDates(const pugi::xml_node& aNode, std::set<std::string>& aDates) {
    for (const char* name : {"tradeDate", "dateTime", "settleDate", "reportDate", "date"})
    {
        const std::string value = aNode.attribute(name).value();

        if (!value.empty())
        {
            aDates.insert(value.substr(0, 8));
        }
    }

    for (const auto child : aNode.children())
    {
        collectDates(child, aDates);
    }
}

/// The attribute names the contract reads from a row element (architecture.md, element table).
const std::vector<std::string> kTradeAttributes{"accountId",
                                                "transactionID",
                                                "tradeID",
                                                "levelOfDetail",
                                                "transactionType",
                                                "assetCategory",
                                                "subCategory",
                                                "symbol",
                                                "description",
                                                "isin",
                                                "currency",
                                                "tradeDate",
                                                "dateTime",
                                                "buySell",
                                                "quantity",
                                                "tradePrice",
                                                "proceeds",
                                                "ibCommission",
                                                "ibCommissionCurrency",
                                                "taxes",
                                                "openCloseIndicator",
                                                "notes",
                                                "multiplier"};

const std::vector<std::string> kCashTransactionAttributes{"accountId",
                                                          "transactionID",
                                                          "actionID",
                                                          "levelOfDetail",
                                                          "type",
                                                          "assetCategory",
                                                          "symbol",
                                                          "description",
                                                          "isin",
                                                          "currency",
                                                          "dateTime",
                                                          "settleDate",
                                                          "reportDate",
                                                          "amount",
                                                          "issuerCountryCode"};

const std::vector<std::string> kCorporateActionAttributes{"accountId",
                                                          "transactionID",
                                                          "actionID",
                                                          "type",
                                                          "assetCategory",
                                                          "symbol",
                                                          "description",
                                                          "actionDescription",
                                                          "isin",
                                                          "currency",
                                                          "dateTime",
                                                          "reportDate",
                                                          "quantity",
                                                          "proceeds",
                                                          "value"};

/// Checks that every attribute of aNames is present on every child element named aElement.
void expectEveryAttribute(const pugi::xml_node& aSection,
                          const char* aElement,
                          const std::vector<std::string>& aNames,
                          const std::string& aLabel) {
    for (const auto row : aSection.children(aElement))
    {
        for (const auto& name : aNames)
        {
            EXPECT_TRUE(row.attribute(name.c_str()))
                << aLabel << " " << aElement << " " << row.attribute("transactionID").value()
                << " lacks " << name;
        }
    }
}

/// Every XML fixture outside file_level/, read as a Flex Query by the parser tests.
const std::vector<std::string>& allXmlFixtures() {
    static const std::vector<std::string> names{"contract_example.xml",
                                                "supported.xml",
                                                "ignored.xml",
                                                "rejected_rows.xml",
                                                "corrections_2024.xml",
                                                "corrections_2025.xml",
                                                "conflicts_2024.xml",
                                                "conflicts_2025.xml",
                                                "unknown_entity.xml",
                                                "timestamps.xml",
                                                "quantities.xml",
                                                "payer_country_2024.xml",
                                                "equivalence/ibkr.xml"};

    return names;
}

TEST(IbkrFixtureTest, FlexStatementsCountMatchesTheStatementsOfEveryXmlFixture) {
    const auto& names = allXmlFixtures();

    for (const auto& name : names)
    {
        const auto document = loadFixture(name);

        const auto root = document.child("FlexQueryResponse");
        ASSERT_TRUE(root) << name;

        const std::size_t declared = root.child("FlexStatements").attribute("count").as_uint();
        EXPECT_EQ(declared, statementsOf(document).size()) << name;
    }
}

TEST(IbkrFixtureTest, ExecutionsOfSupportedXmlAreConsistent) {
    // 6 trades of account U0000000 and 1 of U0000001.
    EXPECT_EQ(checkExecutions("supported.xml"), 7U);
}

TEST(IbkrFixtureTest, ExecutionsOfTimestampsXmlAreConsistent) {
    EXPECT_EQ(checkExecutions("timestamps.xml"), 6U);
}

TEST(IbkrFixtureTest, ExecutionsOfQuantitiesXmlAreConsistent) {
    // Three buys of 0.333333334 at 30 give proceeds -10.00000002; the 31-decimal buy at 10 gives
    // -1.234567890123456789012345678901. Both must hold exactly.
    EXPECT_EQ(checkExecutions("quantities.xml"), 5U);
}

TEST(IbkrFixtureTest, ExecutionsOfEquivalenceXmlAreConsistent) {
    EXPECT_EQ(checkExecutions("equivalence/ibkr.xml"), 2U);
}

TEST(IbkrFixtureTest, ExecutionsOfCorrectionsXmlAreConsistent) {
    // Buy 6001 (3 at USD 20, proceeds -60) is in both files.
    EXPECT_EQ(checkExecutions("corrections_2024.xml"), 1U);
    EXPECT_EQ(checkExecutions("corrections_2025.xml"), 1U);
}

TEST(IbkrFixtureTest, ExecutionsOfConflictsXmlAreConsistent) {
    // Trade 6101 differs only in quantity: 3 x 20 = 60 in 2024, 4 x 20 = 80 in 2025. Each version
    // is consistent on its own, so the parser reads both and the merger finds the conflict.
    EXPECT_EQ(checkExecutions("conflicts_2024.xml"), 1U);
    EXPECT_EQ(checkExecutions("conflicts_2025.xml"), 1U);
}

TEST(IbkrFixtureTest, SupportedXmlRowsHaveEveryAttributeOfTheContract) {
    const auto document = loadFixture("supported.xml");

    const auto statements = statementsOf(document);
    ASSERT_EQ(statements.size(), 2U);

    for (const auto& statement : statements)
    {
        expectEveryAttribute(statement.child("Trades"), "Trade", kTradeAttributes, "supported.xml");
        expectEveryAttribute(statement.child("CashTransactions"),
                             "CashTransaction",
                             kCashTransactionAttributes,
                             "supported.xml");
        expectEveryAttribute(statement.child("CorporateActions"),
                             "CorporateAction",
                             kCorporateActionAttributes,
                             "supported.xml");
    }
}

TEST(IbkrFixtureTest, TransactionIdsAreUniqueAmongTradesOfEachFile) {
    // A Lot reuses the transactionID of its buy, so only Trade elements are compared.
    const auto& names = allXmlFixtures();

    for (const auto& name : names)
    {
        const auto document = loadFixture(name);
        std::set<std::string> ids;
        std::size_t trades = 0;

        for (const auto& statement : statementsOf(document))
        {
            for (const auto trade : statement.child("Trades").children("Trade"))
            {
                ids.insert(trade.attribute("transactionID").value());
                ++trades;
            }
        }

        EXPECT_EQ(ids.size(), trades) << name;
    }
}

TEST(IbkrFixtureTest, EveryIsinHasAValidCheckDigitAndIsOneOfTheKnownIsins) {
    const auto& names = allXmlFixtures();

    std::set<std::string> isins;

    for (const auto& name : names)
    {
        const auto document = loadFixture(name);

        collectIsins(document, isins);
    }

    for (const auto& isin : isins)
    {
        EXPECT_TRUE(hasValidIsinCheckDigit(isin)) << isin;
    }

    const std::set<std::string> known{"US0000000010",
                                      "IE0000000020",
                                      "US0000000036",
                                      "DE0000000041",
                                      "US0000000051",
                                      "LU0000000066",
                                      "FR0000000077",
                                      "NL0000000081",
                                      "BE0000000001",
                                      "CH0000000015",
                                      "DE0000000033",
                                      "DE0000000058",
                                      "FR0000000010",
                                      "IT0000000007"};
    EXPECT_EQ(isins, known);
}

TEST(IbkrFixtureTest, EveryDateIsAWeekdayWithAnEcbUsdRate) {
    // rejected_rows.xml holds malformed dates on purpose and is left out.
    std::vector<std::string> names;

    std::ranges::copy_if(allXmlFixtures(), std::back_inserter(names), [](const std::string& aName) {
        return aName != "rejected_rows.xml";
    });

    std::set<std::string> dates;

    for (const auto& name : names)
    {
        const auto document = loadFixture(name);

        for (const auto& statement : statementsOf(document))
        {
            collectDates(statement, dates);
        }
    }

    ASSERT_FALSE(dates.empty());

    std::map<int, std::string> rateFiles;

    for (const auto& date : dates)
    {
        const int year = std::stoi(date.substr(0, 4));
        const unsigned month = static_cast<unsigned>(std::stoi(date.substr(4, 2)));
        const unsigned day = static_cast<unsigned>(std::stoi(date.substr(6, 2)));

        const std::chrono::year_month_day calendarDate{std::chrono::year{year},
                                                       std::chrono::month{month},
                                                       std::chrono::day{day}};
        ASSERT_TRUE(calendarDate.ok()) << date;

        const auto weekday = std::chrono::weekday{std::chrono::sys_days{calendarDate}};
        EXPECT_NE(weekday, std::chrono::Saturday) << date;
        EXPECT_NE(weekday, std::chrono::Sunday) << date;

        if (!rateFiles.contains(year))
        {
            rateFiles[year] = readFile(ecbFolder() / ("rates_" + std::to_string(year) + ".csv"));
        }

        // A compact rate row is "USD,2024-03-05,1.0865".
        const std::string row =
            "USD," + date.substr(0, 4) + "-" + date.substr(4, 2) + "-" + date.substr(6, 2) + ",";
        EXPECT_NE(rateFiles[year].find(row), std::string::npos) << date << " has no USD rate";
    }
}

TEST(IbkrFixtureTest, FileLevelFixturesHaveTheShapeTheReadmeDescribes) {
    const auto folder = fixtureFolder() / "file_level";

    // Two CSV files: the first character is not the start of an XML element.
    const std::string flexQueryCsv = readFile(folder / "flex_query.csv");
    const std::string activityStatement = readFile(folder / "activity_statement.csv");
    ASSERT_FALSE(flexQueryCsv.empty());
    ASSERT_FALSE(activityStatement.empty());

    EXPECT_NE(flexQueryCsv.front(), '<');
    EXPECT_NE(activityStatement.front(), '<');

    // A well-formed XML file with another root.
    const auto wrongRoot = loadFixture("file_level/wrong_root.xml");
    EXPECT_TRUE(wrongRoot.child("FlexStatementResponse"));
    EXPECT_FALSE(wrongRoot.child("FlexQueryResponse"));

    // A valid file cut off inside Trades: not well-formed.
    pugi::xml_document truncated;
    const auto truncatedResult = truncated.load_file((folder / "truncated.xml").string().c_str());
    EXPECT_NE(truncatedResult.status, pugi::status_ok);

    // A DOCTYPE with an external entity that the file uses.
    const std::string entityText = readFile(folder / "doctype_entity.xml");
    EXPECT_NE(entityText.find("<!DOCTYPE FlexQueryResponse ["), std::string::npos);
    EXPECT_NE(entityText.find("<!ENTITY probe SYSTEM"), std::string::npos);
    EXPECT_NE(entityText.find("&probe;"), std::string::npos);

    // A valid file with no statement.
    const auto empty = loadFixture("file_level/empty_statements.xml");
    EXPECT_EQ(empty.child("FlexQueryResponse").child("FlexStatements").attribute("count").as_int(),
              0);
    EXPECT_TRUE(statementsOf(empty).empty());

    // A valid file that starts with the three bytes of a UTF-8 byte order mark.
    const std::string bom = readFile(folder / "bom.xml");
    ASSERT_GE(bom.size(), 3U);
    EXPECT_EQ(bom.substr(0, 3), "\xEF\xBB\xBF");

    const auto withBom = loadFixture("file_level/bom.xml");
    EXPECT_TRUE(withBom.child("FlexQueryResponse"));
    EXPECT_EQ(statementsOf(withBom).size(), 1U);
}

} // namespace
