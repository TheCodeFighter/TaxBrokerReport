#include "../support/trade_republic_parser_checks.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
using namespace taxbroker;

/// A trade as the parser stores it, from the source row 2: positive units, with the source sign
/// kept in the unit evidence.
TradeTransaction acceptedTrade(TradeSide aSide, Units aUnits, const char* aSourceUnits) {
    return TradeTransaction{
        .mMetadata = {.mSources = {SourceReference{.mSourceRow = 2}}},
        .mTradeSide = aSide,
        .mUnits = aUnits,
        .mUnitEvidence = {UnitSourceEvidence{.mCanonicalValue = aSourceUnits}},
    };
}

ParseResult resultWithTrades(std::vector<TradeTransaction> aTrades) {
    ParseResult result;
    result.mStatement.mTradeInstruments.push_back(
        TradeInstrument{.mTransactions = std::move(aTrades)});

    return result;
}

/// Writes the CSV text to a temporary file named after the running test, returns the rows that
/// test::recordRowsToCheck finds in it, and removes the file again.
std::vector<std::size_t> recordRowsToCheckOf(std::string_view aText) {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const auto path = std::filesystem::temp_directory_path() /
                      ("taxbroker_parser_checks_" + std::string{info->name()} + ".csv");

    {
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        output << aText;
    }

    auto rows = test::recordRowsToCheck(path);

    std::error_code error;
    std::filesystem::remove(path, error);

    return rows;
}

ParseDiagnostic errorOnRow(std::size_t aRow) {
    return ParseDiagnostic{.mSeverity = DiagnosticSeverity::Error, .mRowIndex = aRow};
}

TEST(ParserChecksTest, ASaleWithNegativeSourceUnitsAndABuyWithPositiveSourceUnitsPass) {
    const auto result = resultWithTrades({acceptedTrade(TradeSide::Sell, 300'000'000, "-3"),
                                          acceptedTrade(TradeSide::Buy, 300'000'000, "3")});

    EXPECT_EQ(test::tradeSignFailure(result), std::nullopt);
}

TEST(ParserChecksTest, ASaleStoredWithPositiveSourceUnitsIsReported) {
    const auto result = resultWithTrades({acceptedTrade(TradeSide::Sell, 300'000'000, "3")});

    EXPECT_EQ(test::tradeSignFailure(result),
              "row 2: the source sign of the units does not match the trade side");
}

TEST(ParserChecksTest, ABuyStoredWithNegativeSourceUnitsIsReported) {
    const auto result = resultWithTrades({acceptedTrade(TradeSide::Buy, 300'000'000, "-3")});

    EXPECT_EQ(test::tradeSignFailure(result),
              "row 2: the source sign of the units does not match the trade side");
}

TEST(ParserChecksTest, ATradeWithNegativeStoredUnitsIsReported) {
    const auto result = resultWithTrades({acceptedTrade(TradeSide::Sell, -300'000'000, "-3")});

    EXPECT_EQ(test::tradeSignFailure(result), "row 2: an accepted trade has no positive units");
}

TEST(ParserChecksTest, ATradeWithoutUnitEvidenceIsReported) {
    // A buy of 1 unit whose source sign cannot be checked: no unit evidence.
    const auto result = resultWithTrades({TradeTransaction{
        .mMetadata = {.mSources = {SourceReference{.mSourceRow = 2}}},
        .mTradeSide = TradeSide::Buy,
        .mUnits = 100'000'000, // 1 unit
        .mUnitEvidence = {},
    }});

    EXPECT_EQ(test::tradeSignFailure(result), "row 2: an accepted trade has no unit evidence");
}

TEST(ParserChecksTest, ATradeWithANegativeStoredAmountIsReported) {
    // A sale of 3 units stored with the amount -EUR 3 instead of EUR 3.
    const auto result = resultWithTrades({TradeTransaction{
        .mMetadata = {.mSources = {SourceReference{.mSourceRow = 2}}},
        .mTradeSide = TradeSide::Sell,
        .mUnits = 300'000'000, // 3 units
        .mAmount = -30'000,    // -EUR 3 at 4 decimals
        .mUnitEvidence = {UnitSourceEvidence{.mCanonicalValue = "-3"}},
    }});

    EXPECT_EQ(test::tradeSignFailure(result), "row 2: an accepted trade has no positive amount");
}

TEST(ParserChecksTest, RowsWithAnEventOrAnErrorPass) {
    // Row 2 has a trade, row 3 has an error, row 4 has a trade.
    auto result = resultWithTrades({acceptedTrade(TradeSide::Buy, 100'000'000, "1")});
    result.mStatement.mTradeInstruments.front().mTransactions.push_back(
        TradeTransaction{.mMetadata = {.mSources = {SourceReference{.mSourceRow = 4}}}});
    result.mDiagnostics.push_back(errorOnRow(3));

    EXPECT_EQ(test::droppedRowFailure(result, {2, 3, 4}), std::nullopt);
}

TEST(ParserChecksTest, ARowWithoutAnEventAndWithoutAnErrorIsReported) {
    // Events on rows 2 and 4 and no error at all: row 3 vanished.
    auto result = resultWithTrades({acceptedTrade(TradeSide::Buy, 100'000'000, "1")});
    result.mStatement.mTradeInstruments.front().mTransactions.push_back(
        TradeTransaction{.mMetadata = {.mSources = {SourceReference{.mSourceRow = 4}}}});

    EXPECT_EQ(test::droppedRowFailure(result, {2, 3, 4}),
              "row 3 has neither an event nor an error");
}

TEST(ParserChecksTest, AWarningDoesNotAccountForARow) {
    ParseResult result;
    result.mDiagnostics.push_back(
        ParseDiagnostic{.mSeverity = DiagnosticSeverity::Warning, .mRowIndex = 2});

    EXPECT_EQ(test::droppedRowFailure(result, {2}), "row 2 has neither an event nor an error");
}

TEST(ParserChecksTest, ARowOfEveryOtherEventKindCountsAsKept) {
    // Rows 2 to 6: a dividend, a coupon, a benefit, a private-market event and a split.
    ParseResult result;
    result.mStatement.mDividendInstruments.push_back(
        DividendInstrument{.mTransactions = {DividendTransaction{
                               .mMetadata = {.mSources = {SourceReference{.mSourceRow = 2}}}}}});
    result.mStatement.mInterestInstruments.push_back(
        InterestInstrument{.mTransactions = {InterestTransaction{
                               .mMetadata = {.mSources = {SourceReference{.mSourceRow = 3}}}}}});
    result.mStatement.mBenefitEvents.push_back(
        BenefitEvent{.mMetadata = {.mSources = {SourceReference{.mSourceRow = 4}}}});
    result.mStatement.mPrivateMarketEvents.push_back(
        PrivateMarketEvent{.mMetadata = {.mSources = {SourceReference{.mSourceRow = 5}}}});
    result.mStatement.mTradeInstruments.push_back(
        TradeInstrument{.mCorporateActions = {CorporateAction{
                            .mMetadata = {.mSources = {SourceReference{.mSourceRow = 6}}}}}});

    EXPECT_EQ(test::droppedRowFailure(result, {2, 3, 4, 5, 6}), std::nullopt);
}

TEST(ParserChecksTest, AFileLevelErrorGivesUpTheRowCheck) {
    // An unclosed quote discards every row; the error has no row number.
    ParseResult result;
    result.mDiagnostics.push_back(ParseDiagnostic{.mSeverity = DiagnosticSeverity::Error});

    EXPECT_EQ(test::droppedRowFailure(result, {2, 3}), std::nullopt);
}

TEST(ParserChecksTest, AnIgnoredCashRowIsLeftOutOfTheRowsToCheck) {
    // Row 2 is a card payment the parser ignores on purpose; rows 3 and 4 must be accounted for.
    const auto rows = recordRowsToCheckOf(R"(category,type,name
CASH,CARD_TRANSACTION,Synthetic Merchant
TRADING,BUY,Synthetic Share
CASH,INTEREST_PAYMENT,Synthetic Interest
)");

    EXPECT_EQ(rows, (std::vector<std::size_t>{3, 4}));
}

TEST(ParserChecksTest, AnIgnoredCashTypeOutsideTheCashCategoryIsChecked) {
    // CARD_TRANSACTION is ignored only in the CASH category; in TRADING it must be accounted for.
    const auto rows = recordRowsToCheckOf(R"(category,type,name
TRADING,CARD_TRANSACTION,Synthetic Share
)");

    EXPECT_EQ(rows, (std::vector<std::size_t>{2}));
}

TEST(ParserChecksTest, AnIgnoredCashRowWithTheWrongFieldCountIsChecked) {
    // 4 fields instead of 3: the parser reports the field count, so the row must have an error.
    const auto rows = recordRowsToCheckOf(R"(category,type,name
CASH,CARD_TRANSACTION,Synthetic Merchant,extra
)");

    EXPECT_EQ(rows, (std::vector<std::size_t>{2}));
}

TEST(ParserChecksTest, AFileWithoutATypeColumnHasNoRowsToCheck) {
    // The parser rejects the header as a whole, so there is no row to account for.
    const auto rows = recordRowsToCheckOf(R"(category,name
CASH,Synthetic Merchant
)");

    EXPECT_TRUE(rows.empty());
}

TEST(ParserChecksTest, AFileWithoutACategoryColumnHasNoRowsToCheck) {
    // The parser rejects the header as a whole, so there is no row to account for.
    const auto rows = recordRowsToCheckOf(R"(type,name
CARD_TRANSACTION,Synthetic Merchant
)");

    EXPECT_TRUE(rows.empty());
}

TEST(ParserChecksTest, AFileThatCannotBeReadHasNoRowsToCheck) {
    // No file at this path: the CSV reader throws, and the check has no rows to account for.
    const auto path = std::filesystem::temp_directory_path() /
                      "taxbroker_parser_checks_AFileThatCannotBeReadHasNoRowsToCheck.csv";

    std::error_code error;
    std::filesystem::remove(path, error);

    EXPECT_TRUE(test::recordRowsToCheck(path).empty());
}

} // namespace
