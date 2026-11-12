#include "processors/fee_summary.hpp"

#include "taxbroker/currency.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace taxbroker;

// All amounts, rates, dates and sources in this test suite are synthetic.

Date date(std::string_view aText) {
    return *parseCalendarDate(aText);
}

SourceReference source(Broker aBroker, std::size_t aRow, std::string aTransactionId) {
    return SourceReference{
        .mBroker = aBroker,
        .mFilename = SourceFilename::fromPath("history.csv"),
        .mSourceRow = aRow,
        .mTransactionId = std::move(aTransactionId),
    };
}

ReportingYearLedger ledger2025() {
    return ReportingYearLedger{.mYear = 2025,
                               .mYearStart = date("2025-01-01"),
                               .mYearEnd = date("2025-12-31")};
}

/// Marks the trades of the first instrument, by position, as events of the year.
void addTradesToLedger(ReportingYearLedger& aLedger, std::size_t aCount) {
    for (std::size_t index = 0; index < aCount; ++index)
    {
        aLedger.mSelectedYearEvents.push_back(
            {.mKind = StatementEventKind::Trade, .mInstrumentIndex = 0, .mEventIndex = index});
    }
}

TEST(FeeSummaryTest, DocumentedExampleTotalsFourEuro) {
    // Broker A: EUR 1.25 and USD 2.50 with its own rate 1 EUR = 1.25 USD. Broker B: EUR 0.75.
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-03-03"),
                               .mSources = {source(Broker::TradeRepublic, 2, "a-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = 12'500, // EUR 1.25
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = date("2025-03-04"),
                               .mSources = {source(Broker::TradeRepublic, 3, "a-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = 25'000, // USD 2.50
                 .mExchangeRate =
                     BrokerExchangeRate{.mValue = 125'000'000, // 1 EUR = 1.25 USD
                                        .mDirection = ExchangeRateDirection::ForeignPerEur,
                                        .mVerification =
                                            RateDirectionVerification::VerifiedBySourceSamples},
                 .mCurrency = Currency::USD,
                 .mFeeCurrency = Currency::USD},
                {.mMetadata = {.mTaxDate = date("2025-03-05"),
                               .mSources = {source(Broker::InteractiveBrokers, 2, "b-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mFeePaid = 7'500, // EUR 0.75
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
            },
    }};
    auto ledger = ledger2025();
    addTradesToLedger(ledger, 3);

    const auto summary = summarizeFeesAndBenefits(statement, ledger).mFees;

    ASSERT_EQ(summary.mBrokers.size(), 2U);

    const auto& brokerA = summary.mBrokers.at(0);

    EXPECT_EQ(brokerA.mBroker, Broker::TradeRepublic);
    ASSERT_EQ(brokerA.mByCurrency.size(), 2U);
    EXPECT_EQ(brokerA.mByCurrency.at(0).mCurrency, Currency::EUR);
    EXPECT_EQ(brokerA.mByCurrency.at(0).mCount, 1U);
    EXPECT_EQ(brokerA.mByCurrency.at(0).mTotal, 12'500);
    EXPECT_EQ(brokerA.mByCurrency.at(1).mCurrency, Currency::USD);
    EXPECT_EQ(brokerA.mByCurrency.at(1).mCount, 1U);
    EXPECT_EQ(brokerA.mByCurrency.at(1).mTotal, 25'000);
    // EUR 1.25 + USD 2.50 / 1.25 = EUR 1.25 + EUR 2.00 = EUR 3.25.
    EXPECT_EQ(brokerA.mEurTotal, 32'500);
    EXPECT_EQ(brokerA.mBrokerRateCount, 1U);

    const auto& brokerB = summary.mBrokers.at(1);

    EXPECT_EQ(brokerB.mBroker, Broker::InteractiveBrokers);
    EXPECT_EQ(brokerB.mEurTotal, 7'500);
    EXPECT_EQ(brokerB.mBrokerRateCount, 0U);

    // EUR 3.25 + EUR 0.75 = EUR 4.00.
    EXPECT_EQ(summary.mEurTotal, 40'000);
}

TEST(FeeSummaryTest, MissingBrokerRateLeavesEurTotalsOpen) {
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-03-03"),
                               .mSources = {source(Broker::TradeRepublic, 2, "a-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = 12'500, // EUR 1.25
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = date("2025-03-04"),
                               .mSources = {source(Broker::TradeRepublic, 3, "a-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = 25'000, // USD 2.50, no broker rate
                 .mCurrency = Currency::USD,
                 .mFeeCurrency = Currency::USD},
                {.mMetadata = {.mTaxDate = date("2025-03-05"),
                               .mSources = {source(Broker::InteractiveBrokers, 2, "b-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mFeePaid = 7'500, // EUR 0.75
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
            },
    }};
    auto ledger = ledger2025();
    addTradesToLedger(ledger, 3);

    const auto summary = summarizeFeesAndBenefits(statement, ledger).mFees;

    ASSERT_EQ(summary.mBrokers.size(), 2U);

    // Broker A keeps both subtotals; its EUR total and the overall EUR total are not available.
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mTotal, 12'500);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(1).mTotal, 25'000);
    EXPECT_FALSE(summary.mBrokers.at(0).mEurTotal.has_value());
    EXPECT_EQ(summary.mBrokers.at(0).mBrokerRateCount, 0U);

    // Broker B is complete on its own.
    EXPECT_EQ(summary.mBrokers.at(1).mEurTotal, 7'500);
    EXPECT_FALSE(summary.mEurTotal.has_value());
}

TEST(FeeSummaryTest, UnverifiedBrokerRateIsNotUsed) {
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-03-04"),
                               .mSources = {source(Broker::TradeRepublic, 3, "a-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = 25'000, // USD 2.50
                 .mExchangeRate =
                     BrokerExchangeRate{.mValue = 125'000'000,
                                        .mDirection = ExchangeRateDirection::ForeignPerEur,
                                        .mVerification = RateDirectionVerification::Unverified},
                 .mCurrency = Currency::USD,
                 .mFeeCurrency = Currency::USD},
            },
    }};
    auto ledger = ledger2025();
    addTradesToLedger(ledger, 1);

    const auto summary = summarizeFeesAndBenefits(statement, ledger).mFees;

    ASSERT_EQ(summary.mBrokers.size(), 1U);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mTotal, 25'000);
    EXPECT_FALSE(summary.mBrokers.at(0).mEurTotal.has_value());
    EXPECT_FALSE(summary.mEurTotal.has_value());
}

TEST(FeeSummaryTest, BrokerRateOfAnotherCurrencyIsNotUsed) {
    // The trade is in USD with a USD rate, but its fee is in GBP: the rate is not for the fee.
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-03-04"),
                               .mSources = {source(Broker::TradeRepublic, 3, "a-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = 10'000, // GBP 1.00
                 .mExchangeRate =
                     BrokerExchangeRate{.mValue = 125'000'000,
                                        .mDirection = ExchangeRateDirection::ForeignPerEur,
                                        .mVerification =
                                            RateDirectionVerification::VerifiedBySourceSamples},
                 .mCurrency = Currency::USD,
                 .mFeeCurrency = Currency::GBP},
            },
    }};
    auto ledger = ledger2025();
    addTradesToLedger(ledger, 1);

    const auto summary = summarizeFeesAndBenefits(statement, ledger).mFees;

    ASSERT_EQ(summary.mBrokers.size(), 1U);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mCurrency, Currency::GBP);
    EXPECT_FALSE(summary.mBrokers.at(0).mEurTotal.has_value());
}

TEST(FeeSummaryTest, OnlyEventsOfTheYearListTheLedgerAreCounted) {
    // Three fees of EUR 1.00, but the ledger lists only the middle one as an event of the year.
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2024-12-31"),
                               .mSources = {source(Broker::TradeRepublic, 2, "a-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = 10'000,
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = date("2025-06-01"),
                               .mSources = {source(Broker::TradeRepublic, 3, "a-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = 10'000,
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = date("2026-01-01"),
                               .mSources = {source(Broker::TradeRepublic, 4, "a-3")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = 10'000,
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
            },
    }};
    auto ledger = ledger2025();

    ledger.mSelectedYearEvents.push_back(
        {.mKind = StatementEventKind::Trade, .mInstrumentIndex = 0, .mEventIndex = 1});

    const auto summary = summarizeFeesAndBenefits(statement, ledger).mFees;

    ASSERT_EQ(summary.mBrokers.size(), 1U);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mCount, 1U);
    EXPECT_EQ(summary.mEurTotal, 10'000);
}

TEST(FeeSummaryTest, FeesOfTradesBondRedemptionsAndPrivateMarketEventsAreCounted) {
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-03-03"),
                               .mSources = {source(Broker::TradeRepublic, 2, "t-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = 10'000, // EUR 1.00
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = date("2025-03-04"),
                               .mSources = {source(Broker::TradeRepublic, 3, "t-2")}},
                 .mTradeSide = TradeSide::Sell,
                 .mFeePaid = 10'000, // EUR 1.00
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
            },
    }};
    statement.mPresentation.mBondRedemptions = {
        {.mMetadata = {.mTaxDate = date("2025-04-01"),
                       .mSources = {source(Broker::TradeRepublic, 4, "bond-1")}},
         .mKind = BondRedemptionKind::Payment,
         .mName = "Synthetic Bond",
         .mIsin = "XX0000000002",
         .mFeePaid = 5'000, // EUR 0.50
         .mCurrency = Currency::EUR,
         .mFeeCurrency = Currency::EUR}};
    statement.mPresentation.mPrivateMarketEvents = {
        {.mMetadata = {.mTaxDate = date("2025-05-01"),
                       .mSources = {source(Broker::TradeRepublic, 5, "pm-1")}},
         .mType = PrivateMarketEventType::Buy,
         .mName = "Synthetic Private Market",
         .mAmount = 1'000'000,
         .mFeePaid = 10'000, // EUR 1.00
         .mCurrency = Currency::EUR}};
    auto ledger = ledger2025();
    addTradesToLedger(ledger, 2);
    ledger.mSelectedYearEvents.push_back(
        {.mKind = StatementEventKind::BondRedemption, .mEventIndex = 0});
    ledger.mSelectedYearEvents.push_back(
        {.mKind = StatementEventKind::PrivateMarket, .mEventIndex = 0});

    const auto summary = summarizeFeesAndBenefits(statement, ledger).mFees;

    ASSERT_EQ(summary.mBrokers.size(), 1U);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mCount, 4U);
    // EUR 1.00 + 1.00 + 0.50 + 1.00 = EUR 3.50.
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mTotal, 35'000);
    EXPECT_EQ(summary.mEurTotal, 35'000);
}

TEST(FeeSummaryTest, BondRedemptionFeeInTheAmountsForeignCurrencyConvertsWithItsOwnBrokerRate) {
    // The fee and the payout are both USD, and the redemption has a verified broker rate.
    MergedStatement statement;
    statement.mPresentation.mBondRedemptions = {
        {.mMetadata = {.mTaxDate = date("2025-04-01"),
                       .mSources = {source(Broker::TradeRepublic, 4, "bond-1")}},
         .mKind = BondRedemptionKind::Payment,
         .mName = "Synthetic Bond",
         .mIsin = "XX0000000002",
         .mFeePaid = 5'000, // USD 0.50
         .mExchangeRate =
             BrokerExchangeRate{.mValue = 125'000'000, // 1 EUR = 1.25 USD
                                .mDirection = ExchangeRateDirection::ForeignPerEur,
                                .mVerification =
                                    RateDirectionVerification::VerifiedBySourceSamples},
         .mCurrency = Currency::USD,
         .mFeeCurrency = Currency::USD}};
    auto ledger = ledger2025();

    ledger.mSelectedYearEvents.push_back(
        {.mKind = StatementEventKind::BondRedemption, .mEventIndex = 0});

    const auto summary = summarizeFeesAndBenefits(statement, ledger).mFees;

    ASSERT_EQ(summary.mBrokers.size(), 1U);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mCurrency, Currency::USD);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mTotal, 5'000);
    // USD 0.50 / 1.25 = EUR 0.40.
    EXPECT_EQ(summary.mBrokers.at(0).mEurTotal, 4'000);
    EXPECT_EQ(summary.mBrokers.at(0).mBrokerRateCount, 1U);
    EXPECT_EQ(summary.mEurTotal, 4'000);
}

TEST(FeeSummaryTest, BondRedemptionFeeInEuroOnAForeignPayoutIsNotConverted) {
    // The payout is USD with a verified rate, but the fee column is EUR: EUR 0.50 stays EUR 0.50.
    MergedStatement statement;
    statement.mPresentation.mBondRedemptions = {
        {.mMetadata = {.mTaxDate = date("2025-04-01"),
                       .mSources = {source(Broker::TradeRepublic, 4, "bond-1")}},
         .mKind = BondRedemptionKind::Payment,
         .mName = "Synthetic Bond",
         .mIsin = "XX0000000002",
         .mFeePaid = 5'000, // EUR 0.50
         .mExchangeRate =
             BrokerExchangeRate{.mValue = 125'000'000, // 1 EUR = 1.25 USD
                                .mDirection = ExchangeRateDirection::ForeignPerEur,
                                .mVerification =
                                    RateDirectionVerification::VerifiedBySourceSamples},
         .mCurrency = Currency::USD,
         .mFeeCurrency = Currency::EUR}};
    auto ledger = ledger2025();

    ledger.mSelectedYearEvents.push_back(
        {.mKind = StatementEventKind::BondRedemption, .mEventIndex = 0});

    const auto summary = summarizeFeesAndBenefits(statement, ledger).mFees;

    ASSERT_EQ(summary.mBrokers.size(), 1U);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mCurrency, Currency::EUR);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mTotal, 5'000);
    EXPECT_EQ(summary.mBrokers.at(0).mEurTotal, 5'000);
    EXPECT_EQ(summary.mBrokers.at(0).mBrokerRateCount, 0U);
    EXPECT_EQ(summary.mEurTotal, 5'000);
}

TEST(FeeSummaryTest, BondRedemptionFeeWithUnverifiedRateHasNoEurValue) {
    // The fee and the payout are USD, but the direction of the broker rate is not verified.
    MergedStatement statement;
    statement.mPresentation.mBondRedemptions = {
        {.mMetadata = {.mTaxDate = date("2025-04-01"),
                       .mSources = {source(Broker::TradeRepublic, 4, "bond-1")}},
         .mKind = BondRedemptionKind::Payment,
         .mName = "Synthetic Bond",
         .mIsin = "XX0000000002",
         .mFeePaid = 5'000, // USD 0.50
         .mExchangeRate =
             BrokerExchangeRate{.mValue = 125'000'000,
                                .mDirection = ExchangeRateDirection::ForeignPerEur,
                                .mVerification = RateDirectionVerification::Unverified},
         .mCurrency = Currency::USD,
         .mFeeCurrency = Currency::USD}};
    auto ledger = ledger2025();

    ledger.mSelectedYearEvents.push_back(
        {.mKind = StatementEventKind::BondRedemption, .mEventIndex = 0});

    const auto summary = summarizeFeesAndBenefits(statement, ledger).mFees;

    ASSERT_EQ(summary.mBrokers.size(), 1U);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mTotal, 5'000);
    EXPECT_FALSE(summary.mBrokers.at(0).mEurTotal.has_value());
    EXPECT_EQ(summary.mBrokers.at(0).mBrokerRateCount, 0U);
    EXPECT_FALSE(summary.mEurTotal.has_value());
}

TEST(FeeSummaryTest, ZeroFeeIsNotAFee) {
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions = {{.mMetadata = {.mTaxDate = date("2025-03-03"),
                                         .mSources = {source(Broker::TradeRepublic, 2, "t-1")}},
                           .mTradeSide = TradeSide::Buy,
                           .mFeePaid = 0, // the export had no fee
                           .mCurrency = Currency::EUR,
                           .mFeeCurrency = Currency::EUR}},
    }};
    auto ledger = ledger2025();
    addTradesToLedger(ledger, 1);

    const auto summary = summarizeFeesAndBenefits(statement, ledger);

    EXPECT_TRUE(summary.mFees.mBrokers.empty());
    EXPECT_FALSE(summary.mFees.mEurTotal.has_value());
}

TEST(FeeSummaryTest, BenefitsAreGroupedByBrokerTypeAndCurrency) {
    MergedStatement statement;
    statement.mPresentation.mBenefitEvents = {
        {.mMetadata = {.mTaxDate = date("2025-02-01"),
                       .mSources = {source(Broker::TradeRepublic, 2, "bn-1")}},
         .mType = BenefitType::Saveback,
         .mName = "Synthetic Share",
         .mAmount = 15'000, // EUR 1.50
         .mCurrency = Currency::EUR},
        {.mMetadata = {.mTaxDate = date("2025-03-01"),
                       .mSources = {source(Broker::TradeRepublic, 3, "bn-2")}},
         .mType = BenefitType::Saveback,
         .mName = "Synthetic Share",
         .mAmount = 15'000, // EUR 1.50
         .mCurrency = Currency::EUR},
        {.mMetadata = {.mTaxDate = date("2025-04-01"),
                       .mSources = {source(Broker::TradeRepublic, 4, "bn-3")}},
         .mType = BenefitType::StockPerk,
         .mName = "Synthetic Share",
         .mAmount = 50'000, // EUR 5.00
         .mCurrency = Currency::EUR},
        {.mMetadata = {.mTaxDate = date("2025-05-01"),
                       .mSources = {source(Broker::TradeRepublic, 5, "bn-4")}},
         .mType = BenefitType::Referral,
         .mName = "Synthetic Referral",
         .mAmount = 25'000, // USD 2.50
         .mCurrency = Currency::USD}};
    auto ledger = ledger2025();

    for (std::size_t index = 0; index < 4; ++index)
    {
        ledger.mSelectedYearEvents.push_back(
            {.mKind = StatementEventKind::Benefit, .mEventIndex = index});
    }

    const auto summary = summarizeFeesAndBenefits(statement, ledger).mBenefits;

    ASSERT_EQ(summary.mBrokers.size(), 3U);

    // Rows come by broker, then by type in enum order: saveback, stock perk, referral.
    EXPECT_EQ(summary.mBrokers.at(0).mType, BenefitType::Saveback);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mCount, 2U);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mTotal, 30'000); // EUR 3.00
    EXPECT_EQ(summary.mBrokers.at(0).mEurTotal, 30'000);

    EXPECT_EQ(summary.mBrokers.at(1).mType, BenefitType::StockPerk);
    EXPECT_EQ(summary.mBrokers.at(1).mEurTotal, 50'000); // EUR 5.00

    // A benefit in USD has no broker rate, so its row and the overall total are not available.
    EXPECT_EQ(summary.mBrokers.at(2).mType, BenefitType::Referral);
    EXPECT_EQ(summary.mBrokers.at(2).mByCurrency.at(0).mCurrency, Currency::USD);
    EXPECT_EQ(summary.mBrokers.at(2).mByCurrency.at(0).mTotal, 25'000);
    EXPECT_FALSE(summary.mBrokers.at(2).mEurTotal.has_value());
    EXPECT_FALSE(summary.mEurTotal.has_value());
}

TEST(FeeSummaryTest, SumThatDoesNotFitIsNotAvailable) {
    constexpr Money kHalfPlusOne = std::numeric_limits<Money>::max() / 2 + 1;

    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-03-03"),
                               .mSources = {source(Broker::TradeRepublic, 2, "t-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = kHalfPlusOne,
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = date("2025-03-04"),
                               .mSources = {source(Broker::TradeRepublic, 3, "t-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = kHalfPlusOne,
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
            },
    }};
    auto ledger = ledger2025();
    addTradesToLedger(ledger, 2);

    const auto summary = summarizeFeesAndBenefits(statement, ledger).mFees;

    // Two halves plus one do not fit in 64 bits: the count stays, the sums are not available.
    ASSERT_EQ(summary.mBrokers.size(), 1U);
    EXPECT_EQ(summary.mBrokers.at(0).mByCurrency.at(0).mCount, 2U);
    EXPECT_FALSE(summary.mBrokers.at(0).mByCurrency.at(0).mTotal.has_value());
    EXPECT_FALSE(summary.mBrokers.at(0).mEurTotal.has_value());
    EXPECT_FALSE(summary.mEurTotal.has_value());
}

TEST(FeeSummaryTest, OverallSumThatDoesNotFitIsNotAvailable) {
    constexpr Money kHalfPlusOne = std::numeric_limits<Money>::max() / 2 + 1;

    // Each broker's own total fits, but the two together do not fit in 64 bits.
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-03-03"),
                               .mSources = {source(Broker::TradeRepublic, 2, "a-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = kHalfPlusOne,
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = date("2025-03-04"),
                               .mSources = {source(Broker::InteractiveBrokers, 2, "b-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mFeePaid = kHalfPlusOne,
                 .mCurrency = Currency::EUR,
                 .mFeeCurrency = Currency::EUR},
            },
    }};
    auto ledger = ledger2025();
    addTradesToLedger(ledger, 2);

    const auto summary = summarizeFeesAndBenefits(statement, ledger).mFees;

    ASSERT_EQ(summary.mBrokers.size(), 2U);
    EXPECT_EQ(summary.mBrokers.at(0).mEurTotal, kHalfPlusOne);
    EXPECT_EQ(summary.mBrokers.at(1).mEurTotal, kHalfPlusOne);
    EXPECT_FALSE(summary.mEurTotal.has_value());
}

} // namespace
