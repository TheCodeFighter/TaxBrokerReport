#include "processors/capital_report_gates.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

SourceReference source(std::size_t aRow, std::string aTransactionId) {
    return SourceReference{
        .mBroker = Broker::TradeRepublic,
        .mFilename = SourceFilename::fromPath("history.csv"),
        .mSourceRow = aRow,
        .mTransactionId = std::move(aTransactionId),
    };
}

/// Puts all trades, actions and private-market events in tax-date order. Equal dates keep the
/// order listed in the test.
void orderChronologically(MergedStatement& aStatement) {
    const auto& presentation = aStatement.mPresentation;
    const auto& instruments = presentation.mTradeInstruments;
    auto& order = aStatement.mChronologicalOrder;

    for (std::size_t instrument = 0; instrument < instruments.size(); ++instrument)
    {
        for (std::size_t trade = 0; trade < instruments[instrument].mTransactions.size(); ++trade)
        {
            order.push_back({StatementEventKind::Trade, instrument, trade});
        }

        for (std::size_t action = 0; action < instruments[instrument].mCorporateActions.size();
             ++action)
        {
            order.push_back({StatementEventKind::CorporateAction, instrument, action});
        }
    }

    for (std::size_t event = 0; event < presentation.mPrivateMarketEvents.size(); ++event)
    {
        order.push_back({StatementEventKind::PrivateMarket, std::nullopt, event});
    }

    const auto taxDate = [&presentation](const StatementEventReference& aReference) {
        if (aReference.mKind == StatementEventKind::PrivateMarket)
        {
            return presentation.mPrivateMarketEvents.at(aReference.mEventIndex).mMetadata.mTaxDate;
        }

        const auto& instrument =
            presentation.mTradeInstruments.at(aReference.mInstrumentIndex.value());

        return aReference.mKind == StatementEventKind::Trade
                   ? instrument.mTransactions.at(aReference.mEventIndex).mMetadata.mTaxDate
                   : instrument.mCorporateActions.at(aReference.mEventIndex).mMetadata.mTaxDate;
    };

    std::stable_sort(order.begin(), order.end(), [&taxDate](const auto& aLeft, const auto& aRight) {
        return taxDate(aLeft) < taxDate(aRight);
    });
}

/// Prepares the real reporting-year ledger, runs the matcher, the loss rule (no answers) and the
/// builder, then the gates.
std::vector<ProcessingDiagnostic> checkYear(StatementMergeResult& aMerged, int aYear) {
    orderChronologically(aMerged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});
    const auto& ledger = prepared.mLedger.value();
    const auto fifo = FifoMatcher::match(aMerged.mStatement, ledger);
    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(aMerged.mStatement, ledger, fifo, decisions);
    const auto built = CapitalGainsReportBuilder::build(aMerged.mStatement, ledger, fifo, loss, {});

    return CapitalReportGates::check(aMerged.mStatement, ledger, fifo, loss, built);
}

/// Like checkYear, with confirmed corporate actions that the matcher applies.
std::vector<ProcessingDiagnostic>
checkYearWithActions(StatementMergeResult& aMerged,
                     int aYear,
                     const std::vector<ConfirmedCorporateAction>& aActions) {
    orderChronologically(aMerged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});
    const auto& ledger = prepared.mLedger.value();
    const auto fifo = FifoMatcher::match(aMerged.mStatement, ledger, aActions);
    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(aMerged.mStatement, ledger, fifo, decisions);
    const auto built = CapitalGainsReportBuilder::build(aMerged.mStatement, ledger, fifo, loss, {});

    return CapitalReportGates::check(aMerged.mStatement, ledger, fifo, loss, built);
}

TEST(CapitalReportGatesTest, StockAndFundSalesPassTheClassificationGate) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share",
            .mIsin = "XX0000000001",
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                   .mSources = {source(2, "synthetic-buy-1")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-10"),
                                   .mSources = {source(3, "synthetic-sell-1")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 2'000'000'000, // EUR 20
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Fund",
            .mIsin = "XX0000000002",
            .mAssetClass = AssetClass::Fund,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-11"),
                                   .mSources = {source(4, "synthetic-buy-2")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-11"),
                                   .mSources = {source(5, "synthetic-sell-2")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 2'000'000'000, // EUR 20
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto diagnostics = checkYear(merged, 2024);

    EXPECT_TRUE(diagnostics.empty());
}

TEST(CapitalReportGatesTest, CryptoSaleIsUnsupported) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Coin",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Crypto,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    ASSERT_EQ(diagnostics.size(), 1U);

    const auto& diagnostic = diagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::UnsupportedTaxTreatment);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::UnsupportedTaxTreatmentCapitalClass);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{textParam("assetClass", "crypto")}));
    EXPECT_EQ(diagnostic.mMessage,
              "Sales of the asset class crypto are not supported in the capital-gains report yet, "
              "so this investment's sales cannot be reported. No Doh-KDVP file was written.");
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Synthetic Coin");
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-03-10"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-sell-1")}));
}

TEST(CapitalReportGatesTest, PrivateFundSaleIsUnsupported) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Private Fund",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::PrivateFund,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.at(0).mCode, ProcessingDiagnosticCode::UnsupportedTaxTreatment);
    EXPECT_EQ(diagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("assetClass", "private_fund")}));
    EXPECT_EQ(diagnostics.at(0).mContext.mIsin, "XX0000000001");
}

TEST(CapitalReportGatesTest, BondSaleIsUnsupported) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Bond",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Bond,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.at(0).mCode, ProcessingDiagnosticCode::UnsupportedTaxTreatment);
    EXPECT_EQ(diagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("assetClass", "bond")}));
}

TEST(CapitalReportGatesTest, DerivativeSaleIsUnsupported) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Derivative",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Derivative,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.at(0).mCode, ProcessingDiagnosticCode::UnsupportedTaxTreatment);
    EXPECT_EQ(diagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("assetClass", "derivative")}));
}

TEST(CapitalReportGatesTest, CryptoPurchaseWithoutASaleThisYearIsNotChecked) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Coin",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Crypto,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    EXPECT_TRUE(diagnostics.empty());
}

TEST(CapitalReportGatesTest, UnknownClassWithTwoSalesIsOneUnclassifiedErrorNamingBothSales) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Unknown,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-10"),
                               .mSources = {source(4, "synthetic-sell-2")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    ASSERT_EQ(diagnostics.size(), 1U);

    const auto& diagnostic = diagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::TaxClassificationUnknown);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::TaxClassificationUnknownCapital);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    // The date of the first sale; the sources of both sales.
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-03-10"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-sell-1"),
                                            source(4, "synthetic-sell-2")}));
}

TEST(CapitalReportGatesTest, SplitBeforeTheSaleGivesNoGateError) {
    // The corporate-action stage gives the error of this pool (CorporateActionResolverTest).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-15"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000}, // 1 unit
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    EXPECT_TRUE(diagnostics.empty());
}

TEST(CapitalReportGatesTest, SplitWithoutASaleThisYearGivesNothing) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-15"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000}, // 1 unit
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    EXPECT_TRUE(diagnostics.empty());
}

TEST(CapitalReportGatesTest, SplitAfterTheSaleInTheSameYearGivesNoGateError) {
    // The split is in the selected year, after the sale: still up to 31 December. The
    // corporate-action stage gives the error of this pool (CorporateActionResolverTest).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-09-01"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000}, // 1 unit
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    EXPECT_TRUE(diagnostics.empty());
}

TEST(CapitalReportGatesTest, NonEurPurchaseGivesMissingExchangeRate) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // USD 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::USD},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    ASSERT_EQ(diagnostics.size(), 1U);

    const auto& diagnostic = diagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::MissingExchangeRateTrade);
    EXPECT_EQ(diagnostic.mMessageParams, (std::vector<MessageParam>{textParam("currency", "USD")}));
    EXPECT_EQ(diagnostic.mMessage,
              "A trade of this investment is in USD, not in EUR. Conversion with the official "
              "exchange rate is not supported yet, so its sales cannot be reported. No Doh-KDVP "
              "file was written.");
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    // The date and source of the USD purchase, where the pool stopped.
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2023-01-10"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(2, "synthetic-buy-1")}));
}

TEST(CapitalReportGatesTest, GbpChfAndJpyTradesNameTheirCurrency) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Pound Share",
            .mIsin = "XX0000000001",
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-03-10"),
                                   .mSources = {source(2, "synthetic-sell-1")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 2'000'000'000, // GBP 20
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::GBP},
                },
        },
        {
            .mName = "Synthetic Franc Share",
            .mIsin = "XX0000000002",
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-03-11"),
                                   .mSources = {source(3, "synthetic-sell-2")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 2'000'000'000, // CHF 20
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::CHF},
                },
        },
        {
            .mName = "Synthetic Yen Share",
            .mIsin = "XX0000000003",
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-03-12"),
                                   .mSources = {source(4, "synthetic-sell-3")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 2'000'000'000, // JPY 20
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::JPY},
                },
        },
    };

    const auto diagnostics = checkYear(merged, 2024);

    // FIFO stops each pool at its foreign sale before it looks at the units.
    ASSERT_EQ(diagnostics.size(), 3U);
    EXPECT_EQ(diagnostics.at(0).mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(diagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "GBP")}));
    EXPECT_EQ(diagnostics.at(1).mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(diagnostics.at(1).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "CHF")}));
    EXPECT_EQ(diagnostics.at(2).mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(diagnostics.at(2).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "JPY")}));
}

TEST(CapitalReportGatesTest, NokTradeNamesItsCurrency) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Krone Share",
            .mIsin = "XX0000000001",
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-03-10"),
                                   .mSources = {source(2, "synthetic-sell-1")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 2'000'000'000, // NOK 20
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::NOK},
                },
        },
    };

    const auto diagnostics = checkYear(merged, 2024);

    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.at(0).mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(diagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "NOK")}));
}

TEST(CapitalReportGatesTest, TradeWithoutACurrencyNamesTheCurrencyUnknown) {
    // The parser rejects an unknown currency; a trade built without one keeps the default.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(2, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // 20 in no known currency
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::Unknown},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    ASSERT_EQ(diagnostics.size(), 1U);

    const auto& diagnostic = diagnostics.at(0);

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::MissingExchangeRate);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{textParam("currency", "unknown")}));
}

TEST(CapitalReportGatesTest, AnOpenLossQuestionGivesNoGateError) {
    // 20 - 30 = -10 and 25 - 30 = -5: both sales have a loss and no purchase within 30 days, so
    // the loss rule asks its question and gives the errors itself. The gates add nothing.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-10"),
                               .mSources = {source(4, "synthetic-sell-2")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'500'000'000, // EUR 25
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    EXPECT_TRUE(diagnostics.empty());
}

TEST(CapitalReportGatesTest, ALossBlockedOnlyInPartGivesNoError) {
    // The sale of 15 units at EUR 18 uses 10 units bought at EUR 10 (+EUR 80) and 5 units bought
    // at EUR 20 (-EUR 10). The 2024-03-20 purchase of 8 units blocks the loss of the 5 units, so
    // 5 of the 15 listed units are blocked: the sale is written in two rows, so no error.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-06-01"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 1'000'000'000,     // 10 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2023-07-03"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 2'000'000'000, // EUR 20
                         .mUnits = 1'000'000'000,     // 10 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-03-11"),
                                       .mSources = {source(4, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'800'000'000, // EUR 18
                         .mUnits = 1'500'000'000,     // 15 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-03-20"),
                                       .mSources = {source(5, "synthetic-buy-3")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'800'000'000, // EUR 18
                         .mUnits = 800'000'000,       // 8 units
                         .mCurrency = Currency::EUR},
                    },
            }};

    const auto diagnostics = checkYear(merged, 2024);

    EXPECT_TRUE(diagnostics.empty());
}

TEST(CapitalReportGatesTest, ADecidedSaleInTheBlockedLossListGivesTheUnitsMismatchError) {
    // The builder lists a decided sale as blocked only when the rule blocks more units than the
    // sale has loss units. The gate reports it with the sale's source (row 4, the sale).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-06-01"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 1'000'000'000,     // 10 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2023-07-03"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 2'000'000'000, // EUR 20
                         .mUnits = 1'000'000'000,     // 10 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-03-11"),
                                       .mSources = {source(4, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 1'800'000'000, // EUR 18
                         .mUnits = 1'500'000'000,     // 15 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-03-20"),
                                       .mSources = {source(5, "synthetic-buy-3")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'800'000'000, // EUR 18
                         .mUnits = 800'000'000,       // 8 units
                         .mCurrency = Currency::EUR},
                    },
            }};

    orderChronologically(merged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({2024, merged});
    const auto& ledger = prepared.mLedger.value();
    const auto fifo = FifoMatcher::match(merged.mStatement, ledger);
    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(merged.mStatement, ledger, fifo, decisions);
    CapitalGainsBuildResult built;

    built.mBlocked.push_back({.mIsin = "XX0000000001",
                              .mName = "Synthetic Share",
                              .mReason = KdvpBlockReason::LossNotReportable,
                              .mSales = {{StatementEventKind::Trade, 0, 2}}});

    const auto diagnostics =
        CapitalReportGates::check(merged.mStatement, ledger, fifo, loss, built);

    ASSERT_EQ(diagnostics.size(), 1U);

    const auto& diagnostic = diagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::LossEligibilityUnresolved);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::LossEligibilityUnresolvedUnitsMismatch);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Synthetic Share");
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-03-11"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(4, "synthetic-sell-1")}));
}

TEST(CapitalReportGatesTest, TwoDecemberLossesWithOneMergerGiveOneError) {
    // The merger on 2025-01-10 lies inside the windows of both loss sales (2024-12-16, up to
    // 2025-01-15, and 2024-12-20, up to 2025-01-19). Both sales are blocked, but the merger gets
    // one error, not one per sale.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-16"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20"),
                               .mSources = {source(4, "synthetic-sell-2")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-10"),
                               .mSources = {source(5, "synthetic-merger-1")}},
                 .mType = CorporateActionType::Merger},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    ASSERT_EQ(diagnostics.size(), 1U);

    const auto& diagnostic = diagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionNotApplied);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionNotAppliedLossWindow);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::CorporateAction);
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Synthetic Share");
    // The date and source of the merger, not of either sale.
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2025-01-10"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(5, "synthetic-merger-1")}));
}

TEST(CapitalReportGatesTest, AJanuarySplitInADecemberLossWindowGivesNoGateError) {
    // The split on 2025-01-10 is 21 days after the loss sale of 2024-12-20, inside its window
    // (up to 2025-01-19). The sale stays blocked, but the corporate-action stage asks the split's
    // ratio and shows its error, so the gates add none (maintainer decision, 2026-10-30).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mAssetClass = AssetClass::Stock,
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-06-03"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 10'000'000'000,    // 100 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-12-20"),
                                       .mSources = {source(3, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 800'000'000, // EUR 8
                         .mUnits = 400'000'000,     // 4 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2025-01-15"),
                                       .mSources = {source(5, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 800'000'000, // EUR 8
                         .mUnits = 400'000'000,     // 4 units
                         .mCurrency = Currency::EUR},
                    },
                .mCorporateActions =
                    {
                        {.mMetadata = {.mTaxDate = day("2025-01-10"),
                                       .mSources = {source(4, "synthetic-split-1")}},
                         .mType = CorporateActionType::UnresolvedSplit,
                         .mUnitsDelta = 10'000'000'000}, // 100 units
                    },
            }};

    const auto diagnostics = checkYear(merged, 2024);

    EXPECT_TRUE(diagnostics.empty());
}

TEST(CapitalReportGatesTest, AJanuarySplitAndMergerOnOneDateGiveOneError) {
    // A split row and a merger row on 2025-01-10, inside the window of the loss sale of
    // 2024-12-20 (up to 2025-01-19). A date with a merger row is not asked, so the gates give the
    // error, on the merger row of that date.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-10"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2025-01-10"),
                               .mSources = {source(5, "synthetic-merger-1")}},
                 .mType = CorporateActionType::Merger},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    ASSERT_EQ(diagnostics.size(), 1U);

    const auto& diagnostic = diagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionNotApplied);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionNotAppliedLossWindow);
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2025-01-10"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(5, "synthetic-merger-1")}));
}

TEST(CapitalReportGatesTest, AJanuaryMergerInADecemberLossWindowGivesOneError) {
    // A merger on 2025-01-10, inside the window of the loss sale of 2024-12-20 (up to
    // 2025-01-19). A merger has no ratio, so the units before and after it cannot be compared.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-15"),
                               .mSources = {source(5, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-10"),
                               .mSources = {source(4, "synthetic-merger-1")}},
                 .mType = CorporateActionType::Merger},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    ASSERT_EQ(diagnostics.size(), 1U);

    const auto& diagnostic = diagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionNotApplied);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionNotAppliedLossWindow);
    // The date and source of the merger, not of the sale.
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2025-01-10"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(4, "synthetic-merger-1")}));
}

TEST(CapitalReportGatesTest, IncompleteHistoryAddsNoSecondError) {
    // The FIFO matcher already reports incomplete_history for this sale.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(2, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    EXPECT_TRUE(diagnostics.empty());
}

TEST(CapitalReportGatesTest, CryptoSaleAfterASplitGetsTheClassError) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Coin",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Crypto,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-15"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 100'000'000}, // 1 unit
            },
    }};

    const auto diagnostics = checkYear(merged, 2024);

    // The corporate-action stage gives the error of the stopped pool
    // (CorporateActionResolverTest); the gates give the class error.
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.at(0).mCode, ProcessingDiagnosticCode::UnsupportedTaxTreatment);
}

TEST(CapitalReportGatesTest, PrivateMarketEventsOfTheYearGetOneWarningEach) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mPrivateMarketEvents = {
        {.mMetadata = {.mTaxDate = day("2023-12-31"),
                       .mSources = {source(2, "synthetic-private-buy-1")}},
         .mType = PrivateMarketEventType::Buy,
         .mName = "Synthetic Private Fund",
         .mIsin = "XX0000000009",
         .mAssetClass = AssetClass::PrivateFund,
         .mAmount = -1'000'000, // EUR -100
         .mCurrency = Currency::EUR},
        {.mMetadata = {.mTaxDate = day("2024-01-01"),
                       .mSources = {source(3, "synthetic-private-buy-2")}},
         .mType = PrivateMarketEventType::Buy,
         .mName = "Synthetic Private Fund",
         .mIsin = "XX0000000009",
         .mAssetClass = AssetClass::PrivateFund,
         .mAmount = -1'000'000, // EUR -100
         .mCurrency = Currency::EUR},
        {.mMetadata = {.mTaxDate = day("2024-12-31"),
                       .mSources = {source(4, "synthetic-private-sell-1")}},
         .mType = PrivateMarketEventType::Sell,
         .mName = "",
         .mIsin = std::nullopt,
         .mAssetClass = AssetClass::Unknown,
         .mAmount = 1'500'000, // EUR 150
         .mCurrency = Currency::EUR},
        {.mMetadata = {.mTaxDate = day("2025-01-01"),
                       .mSources = {source(5, "synthetic-private-sell-2")}},
         .mType = PrivateMarketEventType::Sell,
         .mName = "Synthetic Private Fund",
         .mIsin = "XX0000000009",
         .mAssetClass = AssetClass::PrivateFund,
         .mAmount = 1'500'000, // EUR 150
         .mCurrency = Currency::EUR},
    };

    const auto diagnostics = checkYear(merged, 2024);

    // Only the events of 1 January and 31 December 2024; never an error.
    ASSERT_EQ(diagnostics.size(), 2U);

    const auto& buy = diagnostics.at(0);

    EXPECT_EQ(buy.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(buy.mCode, ProcessingDiagnosticCode::UnsupportedTaxTreatment);
    EXPECT_EQ(buy.mMessageKey, MessageKey::UnsupportedTaxTreatmentPrivateMarketSkipped);
    EXPECT_TRUE(buy.mMessageParams.empty());
    EXPECT_EQ(buy.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(buy.mContext.mAffectedReports, (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(buy.mContext.mIsin, "XX0000000009");
    EXPECT_EQ(buy.mContext.mInstrumentName, "Synthetic Private Fund");
    EXPECT_EQ(buy.mContext.mTaxDate, day("2024-01-01"));
    EXPECT_EQ(buy.mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-private-buy-2")}));

    const auto& sale = diagnostics.at(1);

    EXPECT_EQ(sale.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(sale.mMessageKey, MessageKey::UnsupportedTaxTreatmentPrivateMarketSkipped);
    // No ISIN and no name in the source: none in the warning either.
    EXPECT_FALSE(sale.mContext.mIsin.has_value());
    EXPECT_FALSE(sale.mContext.mInstrumentName.has_value());
    EXPECT_EQ(sale.mContext.mTaxDate, day("2024-12-31"));
    EXPECT_EQ(sale.mContext.mSources,
              (std::vector<SourceReference>{source(4, "synthetic-private-sell-1")}));
}

TEST(CapitalReportGatesTest, ASaleOfUnitsChangedByASplitGivesNoGateError) {
    // 1 unit bought in 2022, split 2 / 1 in 2023, 2 units sold in 2024: the rows list the units
    // after the split, so the gate has nothing to report.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-15"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto diagnostics =
        checkYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2023-01-15")}});

    EXPECT_TRUE(diagnostics.empty());
}

TEST(CapitalReportGatesTest, AnAdjustedUnitValueThatDoesNotFitGivesTheUnitValueError) {
    // 1 unit bought at EUR 92,233,720,368, reverse split 1 / 2 on 2024-03-01: 0.5 units at
    // EUR 184,467,440,736 a unit, which a UnitPrice cannot hold.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 9'223'372'036'800'000'000, // EUR 92,233,720,368
                 .mUnits = 100'000'000,                   // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-02"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 9'223'372'036'800'000'000, // EUR 92,233,720,368
                 .mUnits = 50'000'000,                    // 0.5 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto diagnostics =
        checkYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 2},
                               .mEffectiveDate = day("2024-03-01")}});

    ASSERT_EQ(diagnostics.size(), 1U);

    const auto& diagnostic = diagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionUnrepresentable);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionUnrepresentableUnitValue);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    // The date and source of the first sale that uses the unit value.
    EXPECT_EQ(diagnostic.mContext.mTaxDate, day("2024-04-02"));
    EXPECT_EQ(diagnostic.mContext.mSources,
              (std::vector<SourceReference>{source(4, "synthetic-sell-1")}));
}

TEST(CapitalReportGatesTest, AnAppliedSplitInALossWindowGivesNoError) {
    // 3 of 4 units sold at a loss on 2024-11-20, split 2 / 1 applied on 2024-12-01, inside the
    // window up to 2024-12-20, and 6 new units bought on 2024-12-05. In the sale's units the
    // purchase is 3 units (6 / 2), so it blocks the whole loss of 3 units and the sale is
    // written with F10 false: no error.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-11-20"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 300'000'000,     // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-05"),
                               .mSources = {source(5, "synthetic-buy-2")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 400'000'000, // EUR 4
                 .mUnits = 600'000'000,     // 6 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-12-01"),
                               .mSources = {source(4, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto diagnostics =
        checkYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-12-01")}});

    EXPECT_TRUE(diagnostics.empty());
}

} // namespace
