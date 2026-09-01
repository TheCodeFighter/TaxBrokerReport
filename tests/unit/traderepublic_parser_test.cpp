#include "parsers/traderepublic_parser.hpp"
#include "taxbroker/errors.hpp"
#include "taxbroker/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <numeric>
#include <string>
#include <string_view>

namespace {

using namespace taxbroker;

const TradeInstrument* findTradeInstrument(const BrokerStatement& aStatement,
                                           std::string_view aIsin) {
    const auto instrument = std::find_if(
        aStatement.mTradeInstruments.begin(),
        aStatement.mTradeInstruments.end(),
        [&](const TradeInstrument& aTradeInstrument) { return aTradeInstrument.mIsin == aIsin; });

    return instrument == aStatement.mTradeInstruments.end() ? nullptr : &*instrument;
}

const BenefitEvent* findBenefitEvent(const BrokerStatement& aStatement, BenefitType aBenefitType) {
    const auto benefit =
        std::find_if(aStatement.mBenefitEvents.begin(),
                     aStatement.mBenefitEvents.end(),
                     [&](const BenefitEvent& aEvent) { return aEvent.mType == aBenefitType; });

    return benefit == aStatement.mBenefitEvents.end() ? nullptr : &*benefit;
}

const BenefitEvent* findBenefitEvent(const BrokerStatement& aStatement,
                                     std::string_view aTransactionId) {
    const auto benefit = std::find_if(
        aStatement.mBenefitEvents.begin(),
        aStatement.mBenefitEvents.end(),
        [&](const BenefitEvent& aEvent) { return aEvent.mTransactionId == aTransactionId; });

    return benefit == aStatement.mBenefitEvents.end() ? nullptr : &*benefit;
}

std::size_t countWarnings(const ParseResult& aParseResult, WarningCode aWarningCode) {
    return static_cast<std::size_t>(std::count_if(
        aParseResult.mWarnings.begin(),
        aParseResult.mWarnings.end(),
        [&](const ParseWarning& aWarning) { return aWarning.mCode == aWarningCode; }));
}

bool hasWarningContaining(const ParseResult& aParseResult, std::string_view aText) {
    return std::any_of(aParseResult.mWarnings.begin(),
                       aParseResult.mWarnings.end(),
                       [&](const ParseWarning& aWarning) {
                           return aWarning.mMessage.find(aText) != std::string::npos;
                       });
}

std::filesystem::path fixturePath() {
    return std::filesystem::path{__FILE__}.parent_path() / "traderepublic_parser_fixture.csv";
}

ParseResult parseFixture() {
    tr::TradeRepublicParser parser;
    return parser.parse(fixturePath());
}

TEST(TradeRepublicParserTest, PreservesBenefitsWithoutDuplicatingBuyTransactions) {
    const ParseResult parseResult = parseFixture();

    const auto transactionCount =
        std::accumulate(parseResult.mStatement.mTradeInstruments.begin(),
                        parseResult.mStatement.mTradeInstruments.end(),
                        std::size_t{0},
                        [](std::size_t aCount, const TradeInstrument& aInstrument) {
                            return aCount + aInstrument.mTransactions.size();
                        });
    EXPECT_EQ(transactionCount, 3U);

    const auto* stockperkInstrument = findTradeInstrument(parseResult.mStatement, "US0000000001");
    ASSERT_NE(stockperkInstrument, nullptr);
    ASSERT_EQ(stockperkInstrument->mTransactions.size(), 1U);
    EXPECT_EQ(stockperkInstrument->mTransactions.front().mUnits, 9'200'000);
    ASSERT_TRUE(stockperkInstrument->mTransactions.front().mAmount.has_value());
    EXPECT_EQ(*stockperkInstrument->mTransactions.front().mAmount, 153'100);
    EXPECT_EQ(stockperkInstrument->mTransactions.front().mFeePaid, 0);
    EXPECT_EQ(stockperkInstrument->mTransactions.front().mTransactionId, "stockperk-buy");

    const auto* savebackInstrument = findTradeInstrument(parseResult.mStatement, "IE0000000001");
    ASSERT_NE(savebackInstrument, nullptr);
    ASSERT_EQ(savebackInstrument->mTransactions.size(), 1U);
    EXPECT_EQ(savebackInstrument->mTransactions.front().mUnits, 10'932'600);
    ASSERT_TRUE(savebackInstrument->mTransactions.front().mAmount.has_value());
    EXPECT_EQ(*savebackInstrument->mTransactions.front().mAmount, 9'800);
    EXPECT_EQ(savebackInstrument->mTransactions.front().mTransactionId, "saveback-buy");

    ASSERT_EQ(parseResult.mStatement.mBenefitEvents.size(), 3U);

    const auto* stockperk = findBenefitEvent(parseResult.mStatement, BenefitType::Stockperk);
    ASSERT_NE(stockperk, nullptr);
    EXPECT_EQ(stockperk->mAmount, 153'100);
    EXPECT_EQ(stockperk->mCurrency, Currency::EUR);
    EXPECT_EQ(stockperk->mAssetClass, AssetClass::Stock);
    EXPECT_EQ(stockperk->mName, "Example Stock");
    ASSERT_TRUE(stockperk->mIsin.has_value());
    EXPECT_EQ(*stockperk->mIsin, "US0000000001");
    EXPECT_EQ(stockperk->mTransactionId, "stockperk-cash");

    const auto* saveback = findBenefitEvent(parseResult.mStatement, BenefitType::Saveback);
    ASSERT_NE(saveback, nullptr);
    EXPECT_EQ(saveback->mAmount, 9'800);
    EXPECT_EQ(saveback->mCurrency, Currency::EUR);
    EXPECT_EQ(saveback->mAssetClass, AssetClass::Fund);
    EXPECT_EQ(saveback->mName, "Example Fund");
    ASSERT_TRUE(saveback->mIsin.has_value());
    EXPECT_EQ(*saveback->mIsin, "IE0000000001");
    EXPECT_EQ(saveback->mTransactionId, "saveback-cash");

    const auto* unassignedSaveback =
        findBenefitEvent(parseResult.mStatement, "saveback-unassigned");
    ASSERT_NE(unassignedSaveback, nullptr);
    EXPECT_EQ(unassignedSaveback->mType, BenefitType::Saveback);
    EXPECT_EQ(unassignedSaveback->mAmount, 23'400);
    EXPECT_TRUE(unassignedSaveback->mName.empty());
    EXPECT_FALSE(unassignedSaveback->mIsin.has_value());
    EXPECT_EQ(unassignedSaveback->mAssetClass, AssetClass::Unknown);

    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnsupportedRowType), 0U);
    EXPECT_FALSE(hasWarningContaining(parseResult, "STOCKPERK"));
    EXPECT_FALSE(hasWarningContaining(parseResult, "BENEFITS_SAVEBACK"));
}

TEST(TradeRepublicParserTest, PreservesPrivateMarketEventsAndFundExecution) {
    const ParseResult parseResult = parseFixture();

    const auto* privateFund = findTradeInstrument(parseResult.mStatement, "LU0000000001");
    ASSERT_NE(privateFund, nullptr);
    EXPECT_EQ(privateFund->mAssetClass, AssetClass::PrivateFund);
    ASSERT_EQ(privateFund->mTransactions.size(), 1U);
    EXPECT_FALSE(privateFund->mTransactions.front().mAmount.has_value());
    EXPECT_EQ(privateFund->mTransactions.front().mTransactionId, "private-buy");

    ASSERT_EQ(parseResult.mStatement.mPrivateMarketEvents.size(), 1U);
    const auto& prepayment = parseResult.mStatement.mPrivateMarketEvents.front();
    EXPECT_EQ(prepayment.mType, PrivateMarketEventType::Buy);
    EXPECT_EQ(prepayment.mAmount, -3'000'000);
    EXPECT_EQ(prepayment.mFeePaid, 10'000);
    EXPECT_EQ(prepayment.mTransactionId, "private-prepayment");

    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnsupportedAssetClass), 1U);
}

TEST(TradeRepublicParserTest, PreservesUnresolvedSplitWithoutCreatingATrade) {
    const ParseResult parseResult = parseFixture();

    const auto* splitInstrument = findTradeInstrument(parseResult.mStatement, "US0000000002");
    ASSERT_NE(splitInstrument, nullptr);
    EXPECT_TRUE(splitInstrument->mTransactions.empty());
    ASSERT_EQ(splitInstrument->mCorporateActions.size(), 1U);
    EXPECT_EQ(splitInstrument->mCorporateActions.front().mType, CorporateActionType::Split);
    EXPECT_EQ(splitInstrument->mCorporateActions.front().mUnitsDelta, 56'520'000);
    EXPECT_FALSE(splitInstrument->mCorporateActions.front().mRatio.has_value());
    EXPECT_EQ(splitInstrument->mCorporateActions.front().mTransactionId, "split-action");
}

TEST(TradeRepublicParserTest, ReportsUnknownRowsWithSourceLocation) {
    const ParseResult parseResult = parseFixture();

    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnsupportedRowType), 0U);
    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnsupportedAssetClass), 1U);
    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnknownRowType), 1U);
    ASSERT_EQ(parseResult.mWarnings.size(), 2U);
    EXPECT_TRUE(hasWarningContaining(parseResult, "FUTURE_EVENT"));
    EXPECT_EQ(parseResult.mWarnings.back().mRowIndex, 11U);
    EXPECT_EQ(parseResult.mWarnings.back().mSourceFile, fixturePath().string());
}

} // namespace
