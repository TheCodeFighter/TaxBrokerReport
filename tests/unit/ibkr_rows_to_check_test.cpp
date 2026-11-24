#include "../support/ibkr_rows_to_check.hpp"
#include "../support/parser_checks.hpp"
#include "parsers/ibkr_parser.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The rows that the IBKR fuzz target and these tests expect to end as an event or an error. Each
// XML text has one row per line, and line 1 is the root, so the expected lines can be counted.

namespace {
using namespace taxbroker;
using Lines = std::vector<std::size_t>;

std::filesystem::path ibkrFolder() {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / "ibkr";
}

std::filesystem::path seedFolder() {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "fuzz" / "ibkr_seeds";
}

std::string readFile(const std::filesystem::path& aPath) {
    std::ifstream input{aPath, std::ios::binary};

    EXPECT_TRUE(input.is_open()) << aPath.filename().string();

    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/// Parses the fixture and returns what both checks of the fuzz target say about it.
std::vector<std::string> checkMessagesOf(const std::filesystem::path& aPath) {
    const auto result = ibkr::IbkrParser{}.parseFile(aPath, 0);
    const auto text = readFile(aPath);
    std::vector<std::string> messages;

    if (const auto message = test::tradeSignFailure(result))
    {
        messages.push_back(*message);
    }

    if (const auto message = test::droppedRowFailure(result, test::ibkr::rowsToCheck(text)))
    {
        messages.push_back(*message);
    }

    return messages;
}

TEST(IbkrRowsToCheckTest, EveryRowOfTheThreeSectionsIsChecked) {
    // Line 5 a purchase, line 8 a dividend, line 11 a split.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<Trades>
<Trade levelOfDetail="EXECUTION" transactionID="1" assetCategory="STK"/>
</Trades>
<CashTransactions>
<CashTransaction levelOfDetail="DETAIL" transactionID="2" type="Dividends" assetCategory="STK"/>
</CashTransactions>
<CorporateActions>
<CorporateAction transactionID="3" type="FS" assetCategory="STK"/>
</CorporateActions>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), (Lines{5, 8, 11}));
}

TEST(IbkrRowsToCheckTest, AClosedLotAndARowThatIsNotAnExecutionAreLeftOut) {
    // Line 5 is a closed lot (another element), line 6 a trade row of level CLOSED_LOT.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<Trades>
<Lot levelOfDetail="CLOSED_LOT" transactionID="1" assetCategory="STK"/>
<Trade levelOfDetail="CLOSED_LOT" transactionID="1" assetCategory="STK"/>
</Trades>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), Lines{});
}

TEST(IbkrRowsToCheckTest, ATradeWithAnEmptyLevelOfDetailIsChecked) {
    // The parser rejects an empty level, so line 5 must end as an error.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<Trades>
<Trade levelOfDetail="" transactionID="1" assetCategory="STK"/>
</Trades>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), (Lines{5}));
}

TEST(IbkrRowsToCheckTest, ATradeWithoutALevelOfDetailIsChecked) {
    // The parser rejects the row for the missing attribute.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<Trades>
<Trade transactionID="1" assetCategory="STK"/>
</Trades>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), (Lines{5}));
}

TEST(IbkrRowsToCheckTest, ACurrencyConversionIsLeftOutButADerivativeIsChecked) {
    // Line 5 is a currency conversion (counted in a notice). Line 6 is an option, which the
    // parser records as a left-out row, so it must be accounted for.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<Trades>
<Trade levelOfDetail="EXECUTION" transactionID="1" assetCategory="CASH"/>
<Trade levelOfDetail="EXECUTION" transactionID="2" assetCategory="OPT"/>
</Trades>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), (Lines{6}));
}

TEST(IbkrRowsToCheckTest, CryptoWithoutAnIsinIsLeftOutButCryptoWithOneIsChecked) {
    // Line 5: no isin attribute. Line 6: an empty isin. Line 7: a crypto row with an ISIN.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<Trades>
<Trade levelOfDetail="EXECUTION" transactionID="1" assetCategory="CRYPTO"/>
<Trade levelOfDetail="EXECUTION" transactionID="2" assetCategory="CRYPTO" isin=""/>
<Trade levelOfDetail="EXECUTION" transactionID="3" assetCategory="CRYPTO" isin="XS0000000009"/>
</Trades>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), (Lines{7}));
}

TEST(IbkrRowsToCheckTest, ACashRowThatIsNotASingleBookingIsLeftOut) {
    // Line 5 is a total row, with nothing else on it.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<CashTransactions>
<CashTransaction levelOfDetail="SUMMARY"/>
</CashTransactions>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), Lines{});
}

TEST(IbkrRowsToCheckTest, ACashRowOfAnIgnoredTypeIsLeftOut) {
    // Line 5: other fees. Line 6: deposits. Line 7: dividends, which the parser reads.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<CashTransactions>
<CashTransaction levelOfDetail="DETAIL" transactionID="1" type="Other Fees" assetCategory="CASH"/>
<CashTransaction levelOfDetail="DETAIL" transactionID="2" type="Deposits/Withdrawals" assetCategory="CASH"/>
<CashTransaction levelOfDetail="DETAIL" transactionID="3" type="Dividends" assetCategory="STK"/>
</CashTransactions>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), (Lines{7}));
}

TEST(IbkrRowsToCheckTest, ACashRowWithAnEmptyLevelOfDetailIsChecked) {
    // The parser rejects an empty level, so line 5 must end as an error.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<CashTransactions>
<CashTransaction levelOfDetail="" transactionID="1" type="Other Fees" assetCategory="CASH"/>
</CashTransactions>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), (Lines{5}));
}

TEST(IbkrRowsToCheckTest, AnIgnoredCashRowWithoutATransactionIdIsChecked) {
    // The parser rejects line 5 for the missing transactionID before it looks at the type.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<CashTransactions>
<CashTransaction levelOfDetail="DETAIL" type="Other Fees" assetCategory="CASH"/>
</CashTransactions>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), (Lines{5}));
}

TEST(IbkrRowsToCheckTest, ACorporateActionOfADividendTypeIsLeftOut) {
    // Line 5: a cash dividend row (the cash row is the payment). Line 6: a merger.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<CorporateActions>
<CorporateAction transactionID="1" type="CD" assetCategory="STK"/>
<CorporateAction transactionID="2" type="TC" assetCategory="STK"/>
</CorporateActions>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), (Lines{6}));
}

TEST(IbkrRowsToCheckTest, ACorporateActionOfADividendTypeWithoutAnIdIsChecked) {
    // The parser rejects line 5 for the missing transactionID.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<CorporateActions>
<CorporateAction type="CP" assetCategory="STK"/>
</CorporateActions>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), (Lines{5}));
}

TEST(IbkrRowsToCheckTest, ARowOfAnotherSectionOrOutsideAStatementIsNotRead) {
    // Line 5 is a transfer and line 8 is a trade outside every statement.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<Transfers>
<Transfer transactionID="1" assetCategory="STK"/>
</Transfers>
</FlexStatement>
<Trades>
<Trade levelOfDetail="EXECUTION" transactionID="2" assetCategory="STK"/>
</Trades>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), Lines{});
}

TEST(IbkrRowsToCheckTest, ATextThatIsNotXmlHasNoRowsToCheck) {
    EXPECT_EQ(test::ibkr::rowsToCheck("not xml"), Lines{});
}

TEST(IbkrRowsToCheckTest, ADocumentWithADocumentTypeHasNoRowsToCheck) {
    // The parser rejects the whole file, so the row on line 6 is not checked.
    constexpr std::string_view text = R"(<!DOCTYPE FlexQueryResponse [<!ENTITY x "y">]>
<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<Trades>
<Trade levelOfDetail="EXECUTION" transactionID="1" assetCategory="STK"/>
</Trades>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), Lines{});
}

TEST(IbkrRowsToCheckTest, ADocumentWithAnotherRootHasNoRowsToCheck) {
    constexpr std::string_view text = R"(<Other>
<FlexStatements>
<FlexStatement>
<Trades>
<Trade levelOfDetail="EXECUTION" transactionID="1" assetCategory="STK"/>
</Trades>
</FlexStatement>
</FlexStatements>
</Other>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), Lines{});
}

TEST(IbkrRowsToCheckTest, TheRowsOfTwoStatementsAreBothChecked) {
    // One purchase on line 5 and one on line 10.
    constexpr std::string_view text = R"(<FlexQueryResponse>
<FlexStatements>
<FlexStatement>
<Trades>
<Trade levelOfDetail="EXECUTION" transactionID="1" assetCategory="STK"/>
</Trades>
</FlexStatement>
<FlexStatement>
<Trades>
<Trade levelOfDetail="EXECUTION" transactionID="2" assetCategory="STK"/>
</Trades>
</FlexStatement>
</FlexStatements>
</FlexQueryResponse>)";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), (Lines{5, 10}));
}

TEST(IbkrRowsToCheckTest, ARowAfterABlankLineAndAWindowsLineEndingKeepsItsLine) {
    // "\r\n" ends line 1 and a blank line 2 follows, so the trade is on line 6.
    const std::string text =
        "<FlexQueryResponse>\r\n\r\n<FlexStatements>\r\n<FlexStatement>\r\n<Trades>\r\n"
        "<Trade levelOfDetail=\"EXECUTION\" transactionID=\"1\" assetCategory=\"STK\"/>\r\n"
        "</Trades>\r\n</FlexStatement>\r\n</FlexStatements>\r\n</FlexQueryResponse>";

    EXPECT_EQ(test::ibkr::rowsToCheck(text), (Lines{6}));
}

// Each fixture is parsed by the real parser; the sign check and the dropped-row check must find
// nothing, or the fuzz target would stop on a file the unit tests accept.

TEST(IbkrParserChecksTest, TheContractExamplePasses) {
    EXPECT_TRUE(checkMessagesOf(ibkrFolder() / "contract_example.xml").empty());
}

TEST(IbkrParserChecksTest, TheSupportedRowsPass) {
    EXPECT_TRUE(checkMessagesOf(ibkrFolder() / "supported.xml").empty());
}

TEST(IbkrParserChecksTest, TheIgnoredRowsPass) {
    EXPECT_TRUE(checkMessagesOf(ibkrFolder() / "ignored.xml").empty());
}

TEST(IbkrParserChecksTest, TheRejectedRowsPass) {
    EXPECT_TRUE(checkMessagesOf(ibkrFolder() / "rejected_rows.xml").empty());
}

TEST(IbkrParserChecksTest, ThePayerCountryRowsPass) {
    EXPECT_TRUE(checkMessagesOf(ibkrFolder() / "payer_country_2024.xml").empty());
}

TEST(IbkrParserChecksTest, TheQuantityRowsPass) {
    EXPECT_TRUE(checkMessagesOf(ibkrFolder() / "quantities.xml").empty());
}

TEST(IbkrParserChecksTest, TheTimestampRowsPass) {
    EXPECT_TRUE(checkMessagesOf(ibkrFolder() / "timestamps.xml").empty());
}

TEST(IbkrParserChecksTest, ASaleWithAPositiveQuantityIsRejectedAndPasses) {
    // The seed has one SELL row whose quantity has no minus sign. The parser must reject the row
    // (an error on line 6), so no trade with the wrong sign exists.
    const auto path = seedFolder() / "sell_positive_quantity.xml";
    const auto result = ibkr::IbkrParser{}.parseFile(path, 0);

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(error.mField, "quantity");
    EXPECT_EQ(error.mRowIndex, std::optional<std::size_t>{6});

    EXPECT_TRUE(checkMessagesOf(path).empty());
}

TEST(IbkrParserChecksTest, ADividendWithItsTaxIsAccountedForByPaymentRows) {
    // Two cash rows (lines 6 and 7) become payment rows, not dividend events: the dropped-row
    // check must count payment rows as kept.
    const auto path = seedFolder() / "dividend_with_withholding.xml";
    const auto result = ibkr::IbkrParser{}.parseFile(path, 0);

    EXPECT_EQ(result.mStatement.mIncomePaymentRows.size(), 2U);
    EXPECT_TRUE(checkMessagesOf(path).empty());
}

} // namespace
