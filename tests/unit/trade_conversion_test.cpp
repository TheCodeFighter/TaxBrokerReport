#include "processors/trade_conversion.hpp"

#include "processors/quantity_reconciliation.hpp"

#include "taxbroker/currency.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace taxbroker;

// All rates, prices, dates and sources in this test suite are synthetic.

Date date(std::string_view aText) {
    return *parseCalendarDate(aText);
}

/// Marks the rate file of 2025 as loaded, from 2 January to 31 December.
void addFile2025(OfficialRateTable& aTable) {
    aTable.mFiles.push_back(OfficialRateFile{.mYear = 2025,
                                             .mFirstDate = date("2025-01-02"),
                                             .mLastDate = date("2025-12-31")});
}

/// Adds one rate; aValue is foreign units per 1 EUR at 8 decimals.
void addRate(OfficialRateTable& aTable,
             std::string_view aCode,
             std::string_view aDate,
             ExchangeRate aValue,
             std::size_t aRow) {
    aTable.mRates[{std::string{aCode}, date(aDate)}] =
        OfficialRate{.mCurrencyCode = std::string{aCode},
                     .mObservationDate = date(aDate),
                     .mValue = aValue,
                     .mSourceFile = SourceFilename::fromPath("exr_2025.csv"),
                     .mSourceRow = aRow,
                     .mSource = OfficialRateSource::EcbReferenceRateFile,
                     .mOrigin = OfficialRateOrigin::Supplied};
}

SourceReference source(std::size_t aRow, std::string aTransactionId) {
    return SourceReference{
        .mBroker = Broker::TradeRepublic,
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

TEST(TradeConversionTest, ForeignPurchaseAndSaleConvertOnTheirOwnDays) {
    OfficialRateTable table;
    addFile2025(table);
    addRate(table, "USD", "2025-01-10", 125'000'000, 3); // 1 EUR = 1.25 USD
    addRate(table, "USD", "2025-03-10", 110'000'000, 4); // 1 EUR = 1.10 USD
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // USD 30
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::USD},
                {.mMetadata = {.mTaxDate = date("2025-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'300'000'000, // USD 33
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::USD},
            },
    }};

    const auto result = convertCapitalTrades(statement, ledger2025(), table);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mConvertedStatement.has_value());

    const auto& trades =
        result.mConvertedStatement->mPresentation.mTradeInstruments.at(0).mTransactions;

    // USD 30 / 1.25 = EUR 24 and USD 33 / 1.10 = EUR 30.
    EXPECT_EQ(trades.at(0).mUnitPrice, 2'400'000'000);
    EXPECT_EQ(trades.at(0).mCurrency, Currency::EUR);
    EXPECT_EQ(trades.at(1).mUnitPrice, 3'000'000'000);
    EXPECT_EQ(trades.at(1).mCurrency, Currency::EUR);
    // The statement that was given is not changed.
    EXPECT_EQ(statement.mPresentation.mTradeInstruments.at(0).mTransactions.at(0).mUnitPrice,
              3'000'000'000);
    EXPECT_EQ(statement.mPresentation.mTradeInstruments.at(0).mTransactions.at(0).mCurrency,
              Currency::USD);

    ASSERT_EQ(result.mConverted.size(), 2U);
    EXPECT_EQ(result.mConverted.at(0).mTrade,
              (StatementEventReference{.mKind = StatementEventKind::Trade,
                                       .mInstrumentIndex = 0,
                                       .mEventIndex = 0}));
    EXPECT_EQ(result.mConverted.at(0).mOriginalCurrency, Currency::USD);
    EXPECT_EQ(result.mConverted.at(0).mOriginalUnitPrice, 3'000'000'000);
    EXPECT_EQ(result.mConverted.at(0).mEurUnitPrice, 2'400'000'000);
    EXPECT_EQ(result.mConverted.at(1).mEurUnitPrice, 3'000'000'000);

    // One used rate for each day, in the order of the days.
    ASSERT_EQ(result.mUsedRates.size(), 2U);
    EXPECT_EQ(result.mUsedRates.at(0).mTaxDate, date("2025-01-10"));
    EXPECT_EQ(result.mUsedRates.at(0).mRate.mValue, 125'000'000);
    EXPECT_EQ(result.mUsedRates.at(0).mRate.mSourceRow, 3U);
    EXPECT_EQ(result.mUsedRates.at(1).mTaxDate, date("2025-03-10"));
    EXPECT_EQ(result.mUsedRates.at(1).mRate.mSourceRow, 4U);
}

TEST(TradeConversionTest, TwoInvestmentsTradedOnOneCurrencyAndDayUseOneRate) {
    OfficialRateTable table;
    addFile2025(table);
    addRate(table, "USD", "2025-01-10", 125'000'000, 3); // 1 EUR = 1.25 USD
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share One",
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = date("2025-01-10"),
                                   .mSources = {source(2, "synthetic-buy-1")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 3'000'000'000, // USD 30
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::USD},
                },
        },
        {
            .mName = "Synthetic Share Two",
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = date("2025-01-10"),
                                   .mSources = {source(3, "synthetic-buy-2")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 3'000'000'000, // USD 30
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::USD},
                },
        },
    };

    const auto result = convertCapitalTrades(statement, ledger2025(), table);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_TRUE(result.mConvertedStatement.has_value());

    const auto& instruments = result.mConvertedStatement->mPresentation.mTradeInstruments;

    // USD 30 / 1.25 = EUR 24 for both investments.
    EXPECT_EQ(instruments.at(0).mTransactions.at(0).mUnitPrice, 2'400'000'000);
    EXPECT_EQ(instruments.at(0).mTransactions.at(0).mCurrency, Currency::EUR);
    EXPECT_EQ(instruments.at(1).mTransactions.at(0).mUnitPrice, 2'400'000'000);
    EXPECT_EQ(instruments.at(1).mTransactions.at(0).mCurrency, Currency::EUR);

    // Two conversions, but the summary lists the rate of that currency and day once.
    EXPECT_EQ(result.mConverted.size(), 2U);
    ASSERT_EQ(result.mUsedRates.size(), 1U);
    EXPECT_EQ(result.mUsedRates.at(0).mTaxDate, date("2025-01-10"));
    EXPECT_EQ(result.mUsedRates.at(0).mRate.mValue, 125'000'000);
}

TEST(TradeConversionTest, OnlyTheTradeWithARateIsConverted) {
    OfficialRateTable table;
    addFile2025(table);
    addRate(table, "USD", "2025-01-10", 125'000'000, 3); // 1 EUR = 1.25 USD
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // USD 30
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::USD},
                // The loaded file has no rate for this day or the four days before it.
                {.mMetadata = {.mTaxDate = date("2025-06-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'300'000'000, // USD 33
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::USD},
            },
    }};

    const auto result = convertCapitalTrades(statement, ledger2025(), table);

    ASSERT_TRUE(result.mConvertedStatement.has_value());

    const auto& trades =
        result.mConvertedStatement->mPresentation.mTradeInstruments.at(0).mTransactions;

    // The purchase: USD 30 / 1.25 = EUR 24. The sale keeps its USD price, so its pool stops.
    EXPECT_EQ(trades.at(0).mUnitPrice, 2'400'000'000);
    EXPECT_EQ(trades.at(0).mCurrency, Currency::EUR);
    EXPECT_EQ(trades.at(1).mUnitPrice, 3'300'000'000);
    EXPECT_EQ(trades.at(1).mCurrency, Currency::USD);

    EXPECT_EQ(result.mConverted.size(), 1U);
    EXPECT_EQ(result.mUsedRates.size(), 1U);
    EXPECT_EQ(result.mRates.mMissing.size(), 1U);
    EXPECT_NE(result.mRates.findMissing(Currency::USD, date("2025-06-10")), nullptr);
    EXPECT_EQ(result.mRates.findMissing(Currency::USD, date("2025-01-10")), nullptr);
}

TEST(TradeConversionTest, AConvertedSaleHasAKnownValueForQuantityCorrections) {
    OfficialRateTable table;
    addFile2025(table);
    addRate(table, "USD", "2025-03-10", 110'000'000, 3); // 1 EUR = 1.10 USD
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'300'000'000, // USD 33
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::USD},
            },
    }};

    // A sale in USD has an unknown value, so a shortage of it is never reconciled.
    EXPECT_EQ(
        correctionUnitValue(statement.mPresentation.mTradeInstruments.at(0).mTransactions.at(0),
                            {}),
        std::nullopt);

    const auto result = convertCapitalTrades(statement, ledger2025(), table);

    ASSERT_TRUE(result.mConvertedStatement.has_value());

    // USD 33 / 1.10 = EUR 30 per unit.
    const auto value = correctionUnitValue(
        result.mConvertedStatement->mPresentation.mTradeInstruments.at(0).mTransactions.at(0),
        {});

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->numerator(), WideInteger{30});
    EXPECT_EQ(value->denominator(), WideInteger{1});
}

TEST(TradeConversionTest, ThePriceIsRoundedOnceToEightDecimals) {
    OfficialRateTable table;
    addFile2025(table);
    addRate(table, "USD", "2025-01-10", 108'450'000, 3); // 1 EUR = 1.0845 USD
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // USD 10.00000000
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::USD},
            },
    }};

    const auto result = convertCapitalTrades(statement, ledger2025(), table);

    ASSERT_TRUE(result.mConvertedStatement.has_value());
    // 10 / 1.0845 = 9.220839096..., which is EUR 9.22083910 at 8 decimals.
    EXPECT_EQ(result.mConvertedStatement->mPresentation.mTradeInstruments.at(0)
                  .mTransactions.at(0)
                  .mUnitPrice,
              922'083'910);
}

TEST(TradeConversionTest, EurTradesNeedNoRateAndGiveNoCopy) {
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = convertCapitalTrades(statement, ledger2025(), OfficialRateTable{});

    EXPECT_FALSE(result.mConvertedStatement.has_value());
    EXPECT_TRUE(result.mConverted.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mRates.mMissing.empty());
    // Without a copy, the capital stages read the statement they were given.
    EXPECT_EQ(&result.statement(statement), &statement);
}

TEST(TradeConversionTest, ATradeAfterTheYearNeedsNoRate) {
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2026-01-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // USD 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::USD},
            },
    }};

    const auto result = convertCapitalTrades(statement, ledger2025(), OfficialRateTable{});

    // Only the units of a trade after 31 December count (the 30-day loss rule), not its price.
    EXPECT_FALSE(result.mConvertedStatement.has_value());
    EXPECT_TRUE(result.mRates.mMissing.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TradeConversionTest, ALeftOutInvestmentNeedsNoRate) {
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // USD 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::USD},
            },
    }};
    auto ledger = ledger2025();

    ledger.mLeftOutIsins = {"XX0000000001"};

    const auto result = convertCapitalTrades(statement, ledger, OfficialRateTable{});

    EXPECT_FALSE(result.mConvertedStatement.has_value());
    EXPECT_TRUE(result.mRates.mMissing.empty());
}

TEST(TradeConversionTest, ATradeInAnUnknownCurrencyIsNotConverted) {
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // 10 in no known currency
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::Unknown},
            },
    }};

    const auto result = convertCapitalTrades(statement, ledger2025(), OfficialRateTable{});

    EXPECT_FALSE(result.mConvertedStatement.has_value());
    EXPECT_TRUE(result.mRates.mMissing.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TradeConversionTest, ATradeWithoutARateStaysForeignAndKeepsTheReason) {
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // USD 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::USD},
            },
    }};

    // No rate file is loaded.
    const auto result = convertCapitalTrades(statement, ledger2025(), OfficialRateTable{});

    EXPECT_FALSE(result.mConvertedStatement.has_value());
    // The error is not in mDiagnostics: the gate gives it for the investment.
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto* missing = result.mRates.findMissing(Currency::USD, date("2025-01-10"));

    ASSERT_NE(missing, nullptr);
    EXPECT_EQ(missing->mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(missing->mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(missing->mMessageKey, MessageKey::MissingExchangeRateNoRateFile);
    EXPECT_EQ(missing->mContext.mSources,
              (std::vector<SourceReference>{source(2, "synthetic-buy-1")}));
}

TEST(TradeConversionTest, ARateFromAnEarlierDayConvertsWithOneWarning) {
    OfficialRateTable table;
    addFile2025(table);
    addRate(table, "USD", "2025-01-10", 125'000'000, 3); // Friday, 1 EUR = 1.25 USD
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-01-11"),
                               .mSources = {source(2, "synthetic-buy-1")}}, // Saturday
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // USD 30
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::USD},
            },
    }};

    const auto result = convertCapitalTrades(statement, ledger2025(), table);

    ASSERT_TRUE(result.mConvertedStatement.has_value());
    // USD 30 / 1.25 = EUR 24.
    EXPECT_EQ(result.mConvertedStatement->mPresentation.mTradeInstruments.at(0)
                  .mTransactions.at(0)
                  .mUnitPrice,
              2'400'000'000);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ProcessingDiagnosticCode::OfficialRateFromNearbyDate);
    ASSERT_EQ(result.mUsedRates.size(), 1U);
    EXPECT_EQ(result.mUsedRates.at(0).mTaxDate, date("2025-01-11"));
    EXPECT_EQ(result.mUsedRates.at(0).mRate.mObservationDate, date("2025-01-10"));
}

TEST(TradeConversionTest, APriceThatDoesNotFitStaysForeignWithAnOverflowError) {
    OfficialRateTable table;
    addFile2025(table);
    addRate(table, "USD", "2025-01-10", 1, 3); // 1 EUR = 0.00000001 USD
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = date("2025-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 // USD 1'000'000'000.00000000; divided by 0.00000001 it is 10^17 EUR, which is
                 // 10^25 in 8 decimals and does not fit 64 bits.
                 .mUnitPrice = 100'000'000'000'000'000,
                 .mUnits = 100'000'000, // 1 unit
                 .mCurrency = Currency::USD},
            },
    }};

    const auto result = convertCapitalTrades(statement, ledger2025(), table);

    EXPECT_FALSE(result.mConvertedStatement.has_value());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::ArithmeticOverflow);
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(2, "synthetic-buy-1")}));
}

} // namespace
