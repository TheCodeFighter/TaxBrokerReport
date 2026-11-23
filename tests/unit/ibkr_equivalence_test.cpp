#include "parsers/ibkr_parser.hpp"
#include "parsers/traderepublic_parser.hpp"
#include "processors/statement_merger.hpp"
#include "taxbroker/currency.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// The same events in a Trade Republic CSV and an Interactive Brokers XML (tests/test_data/ibkr/
// equivalence, with the table of equal fields in its README) must be equal after the parser and
// the merger, apart from the broker, the transaction ID, the source, the name and the fields that
// the README names. The expected values are written here, not read from either result.

namespace {

using namespace taxbroker;

std::filesystem::path equivalenceFolder() {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / "ibkr" /
           "equivalence";
}

/// Parses one file and merges it alone, as a run with only this file does.
StatementMergeResult mergeOneFile(ParseResult aResult) {
    std::vector<StatementMergeInput> inputs;

    inputs.push_back({0, std::move(aResult)});

    return DeterministicStatementMerger{}.merge(std::move(inputs));
}

StatementMergeResult mergedTradeRepublic() {
    tr::TradeRepublicParser parser;

    return mergeOneFile(parser.parseFile(equivalenceFolder() / "tr.csv", 0));
}

StatementMergeResult mergedInteractiveBrokers() {
    ibkr::IbkrParser parser;

    return mergeOneFile(parser.parseFile(equivalenceFolder() / "ibkr.xml", 0));
}

/// Merged once for the whole suite; the files are small, but the parsers are not free.
const StatementMergeResult& tradeRepublic() {
    static const StatementMergeResult merged = mergedTradeRepublic();

    return merged;
}

const StatementMergeResult& interactiveBrokers() {
    static const StatementMergeResult merged = mergedInteractiveBrokers();

    return merged;
}

SourceTimestamp
utc(int aYear, unsigned aMonth, unsigned aDay, int aHour, int aMinute, int aSecond) {
    using namespace std::chrono;

    return SourceTimestamp{sys_days{year{aYear} / month{aMonth} / day{aDay}} + hours{aHour} +
                           minutes{aMinute} + seconds{aSecond}};
}

Date date(std::string_view aText) {
    return *parseCalendarDate(aText);
}

const TradeInstrument* findTradeInstrument(const StatementMergeResult& aMerged,
                                           std::string_view aIsin) {
    const auto& instruments = aMerged.mStatement.mPresentation.mTradeInstruments;
    const auto found = std::find_if(
        instruments.begin(),
        instruments.end(),
        [aIsin](const TradeInstrument& aInstrument) { return aInstrument.mIsin == aIsin; });

    return found == instruments.end() ? nullptr : &*found;
}

const DividendInstrument* findDividendInstrument(const StatementMergeResult& aMerged,
                                                 std::string_view aIsin) {
    const auto& instruments = aMerged.mStatement.mPresentation.mDividendInstruments;
    const auto found = std::find_if(
        instruments.begin(),
        instruments.end(),
        [aIsin](const DividendInstrument& aInstrument) { return aInstrument.mIsin == aIsin; });

    return found == instruments.end() ? nullptr : &*found;
}

/// The checks of the trades are the same for both brokers: one table, two runs.
void expectTheTrades(const StatementMergeResult& aMerged) {
    const auto* instrument = findTradeInstrument(aMerged, "FR0000000077");

    ASSERT_NE(instrument, nullptr);
    EXPECT_EQ(instrument->mAssetClass, AssetClass::Stock);
    ASSERT_EQ(instrument->mTransactions.size(), 2U);

    const auto& buy = instrument->mTransactions.at(0);

    EXPECT_EQ(buy.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(buy.mUnits, 500'000'000);       // 5 units
    EXPECT_EQ(buy.mUnitPrice, 2'000'000'000); // EUR 20
    EXPECT_EQ(buy.mFeePaid, 10'000);          // EUR 1.00
    EXPECT_EQ(buy.mFeeCurrency, Currency::EUR);
    EXPECT_EQ(buy.mCurrency, Currency::EUR);
    EXPECT_EQ(buy.mMetadata.mTaxDate, date("2024-03-05"));
    EXPECT_EQ(buy.mMetadata.mSourceTimestamp, utc(2024, 3, 5, 14, 42, 10));
    EXPECT_EQ(buy.mTransactionTaxPaid, 0);

    const auto& sell = instrument->mTransactions.at(1);

    EXPECT_EQ(sell.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(sell.mUnits, 200'000'000);       // 2 units
    EXPECT_EQ(sell.mUnitPrice, 2'500'000'000); // EUR 25
    EXPECT_EQ(sell.mFeePaid, 10'000);          // EUR 1.00
    EXPECT_EQ(sell.mFeeCurrency, Currency::EUR);
    EXPECT_EQ(sell.mCurrency, Currency::EUR);
    EXPECT_EQ(sell.mMetadata.mTaxDate, date("2024-06-10"));
    EXPECT_EQ(sell.mMetadata.mSourceTimestamp, utc(2024, 6, 10, 14, 15, 0));
    EXPECT_EQ(sell.mTransactionTaxPaid, 0);
}

TEST(IbkrEquivalenceTest, TheTradesOfTradeRepublicAreTheExpectedOnes) {
    expectTheTrades(tradeRepublic());
}

TEST(IbkrEquivalenceTest, TheTradesOfInteractiveBrokersAreTheExpectedOnes) {
    expectTheTrades(interactiveBrokers());
}

/// The EUR dividend with tax: Trade Republic has one row, IBKR a dividend row and a tax row.
void expectTheEuroDividend(const StatementMergeResult& aMerged) {
    const auto* instrument = findDividendInstrument(aMerged, "FR0000000077");

    ASSERT_NE(instrument, nullptr);
    ASSERT_EQ(instrument->mTransactions.size(), 1U);

    const auto& dividend = instrument->mTransactions.front();

    EXPECT_EQ(dividend.mMetadata.mTaxDate, date("2024-04-15"));
    EXPECT_EQ(dividend.mGrossAmount, 100'000); // EUR 10.00
    ASSERT_TRUE(dividend.mTaxPaid.has_value());
    EXPECT_EQ(*dividend.mTaxPaid, 15'000); // EUR 1.50
    EXPECT_EQ(dividend.mCurrency, Currency::EUR);
    EXPECT_EQ(dividend.mTaxCurrency, Currency::EUR);
}

TEST(IbkrEquivalenceTest, TheEuroDividendOfTradeRepublicIsTheExpectedOne) {
    expectTheEuroDividend(tradeRepublic());
}

TEST(IbkrEquivalenceTest, TheEuroDividendOfInteractiveBrokersIsTheExpectedOne) {
    expectTheEuroDividend(interactiveBrokers());
}

/// The USD dividend without tax. Trade Republic also keeps its EUR rate, IBKR keeps none.
void expectTheDollarDividend(const StatementMergeResult& aMerged) {
    const auto* instrument = findDividendInstrument(aMerged, "US0000000010");

    ASSERT_NE(instrument, nullptr);
    ASSERT_EQ(instrument->mTransactions.size(), 1U);

    const auto& dividend = instrument->mTransactions.front();

    EXPECT_EQ(dividend.mMetadata.mTaxDate, date("2024-05-16"));
    EXPECT_EQ(dividend.mGrossAmount, 100'000); // USD 10.00
    EXPECT_EQ(dividend.mCurrency, Currency::USD);
    EXPECT_FALSE(dividend.mTaxPaid.has_value()); // blank: no foreign tax paid
}

TEST(IbkrEquivalenceTest, TheDollarDividendOfTradeRepublicIsTheExpectedOne) {
    expectTheDollarDividend(tradeRepublic());
}

TEST(IbkrEquivalenceTest, TheDollarDividendOfInteractiveBrokersIsTheExpectedOne) {
    expectTheDollarDividend(interactiveBrokers());
}

/// The interest on the broker's cash, before the date when Trade Republic moved it to a bank.
void expectTheInterest(const StatementMergeResult& aMerged) {
    ASSERT_EQ(aMerged.mStatement.mPresentation.mInterestInstruments.size(), 1U);

    const auto& instrument = aMerged.mStatement.mPresentation.mInterestInstruments.front();

    EXPECT_EQ(instrument.mInterestType, InterestType::BrokerInterest);
    EXPECT_FALSE(instrument.mIsin.has_value());
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& interest = instrument.mTransactions.front();

    EXPECT_EQ(interest.mMetadata.mTaxDate, date("2023-11-15"));
    EXPECT_EQ(interest.mGrossAmount, 25'000); // EUR 2.50
    EXPECT_EQ(interest.mCurrency, Currency::EUR);
    EXPECT_EQ(interest.mCashHolder, CashHolder::Broker);
}

TEST(IbkrEquivalenceTest, TheInterestOfTradeRepublicIsTheExpectedOne) {
    expectTheInterest(tradeRepublic());
}

TEST(IbkrEquivalenceTest, TheInterestOfInteractiveBrokersIsTheExpectedOne) {
    expectTheInterest(interactiveBrokers());
}

TEST(IbkrEquivalenceTest, ABrokerHasOnlyTheDifferencesTheReadmeNames) {
    // The payer differs on purpose: Trade Republic has none in the export, IBKR names its entity.
    const auto& tradeRepublicInterest =
        tradeRepublic().mStatement.mPresentation.mInterestInstruments.front().mTransactions.front();
    const auto& ibkrInterest = interactiveBrokers()
                                   .mStatement.mPresentation.mInterestInstruments.front()
                                   .mTransactions.front();

    EXPECT_FALSE(tradeRepublicInterest.mPayerCode.has_value());
    EXPECT_EQ(ibkrInterest.mPayerCode, "IB-IE");

    // Trade Republic keeps a timestamp on the dividend; IBKR has none for a cash row.
    EXPECT_TRUE(tradeRepublic()
                    .mStatement.mPresentation.mDividendInstruments.front()
                    .mTransactions.front()
                    .mMetadata.mSourceTimestamp.has_value());
    EXPECT_FALSE(interactiveBrokers()
                     .mStatement.mPresentation.mDividendInstruments.front()
                     .mTransactions.front()
                     .mMetadata.mSourceTimestamp.has_value());
}

TEST(IbkrEquivalenceTest, TheExportCountryOfADividendIsADifferenceTheReadmeNames) {
    // Trade Republic has no country in the export; IBKR names the country of each payment.
    const auto* tradeRepublicEuro = findDividendInstrument(tradeRepublic(), "FR0000000077");
    const auto* ibkrEuro = findDividendInstrument(interactiveBrokers(), "FR0000000077");
    const auto* tradeRepublicDollar = findDividendInstrument(tradeRepublic(), "US0000000010");
    const auto* ibkrDollar = findDividendInstrument(interactiveBrokers(), "US0000000010");

    ASSERT_NE(tradeRepublicEuro, nullptr);
    ASSERT_NE(ibkrEuro, nullptr);
    ASSERT_NE(tradeRepublicDollar, nullptr);
    ASSERT_NE(ibkrDollar, nullptr);

    EXPECT_FALSE(tradeRepublicEuro->mTransactions.front().mPayerCountry.has_value());
    EXPECT_EQ(ibkrEuro->mTransactions.front().mPayerCountry, "FR");
    EXPECT_FALSE(tradeRepublicDollar->mTransactions.front().mPayerCountry.has_value());
    EXPECT_EQ(ibkrDollar->mTransactions.front().mPayerCountry, "US");
}

} // namespace
