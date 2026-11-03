#include "processors/capital_gains_report_builder.hpp"
#include "processors/capital_report_gates.hpp"
#include "processors/corporate_actions.hpp"
#include "processors/fifo_matcher.hpp"
#include "processors/loss_eligibility.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;

// An instrument that a tax rule or the user left out of capital gains (its ISIN is in the ledger's
// left-out set) is skipped by every capital stage. Each test runs its stage twice on the same
// statement: with an empty set to show what the stage would do, and with the ISIN in it.

constexpr Units kOneUnit = 100'000'000; // Units have eight decimals

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

/// Puts all trades and actions in tax-date order. Equal dates keep the order listed in the test.
void orderChronologically(MergedStatement& aStatement) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
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

    const auto taxDate = [&instruments](const StatementEventReference& aReference) {
        const auto& instrument = instruments.at(aReference.mInstrumentIndex.value());

        return aReference.mKind == StatementEventKind::Trade
                   ? instrument.mTransactions.at(aReference.mEventIndex).mMetadata.mTaxDate
                   : instrument.mCorporateActions.at(aReference.mEventIndex).mMetadata.mTaxDate;
    };

    std::stable_sort(order.begin(), order.end(), [&taxDate](const auto& aLeft, const auto& aRight) {
        return taxDate(aLeft) < taxDate(aRight);
    });
}

/// The ledger of 2024 for the statement, with the first instrument's ISIN in the left-out set or
/// not.
ReportingYearLedger ledgerOf(StatementMergeResult& aMerged, bool aLeaveFirstOut) {
    orderChronologically(aMerged.mStatement);

    auto ledger = TaxProcessor{}.prepareReportingYear({2024, aMerged}).mLedger.value();

    if (aLeaveFirstOut)
    {
        ledger.mLeftOutIsins = {"XX0000000001"};
    }

    return ledger;
}

/// Every capital stage on the statement, with the first instrument left out or not.
struct Stages {
    FifoMatchingResult mFifo;
    LossEligibilityResult mLoss;
    CapitalGainsBuildResult mBuilt;
};

Stages runStages(StatementMergeResult& aMerged, bool aLeaveFirstOut) {
    const auto ledger = ledgerOf(aMerged, aLeaveFirstOut);
    RunDecisions decisions;
    Stages stages;

    stages.mFifo = FifoMatcher::match(aMerged.mStatement, ledger);
    stages.mLoss =
        LossEligibilityChecker::check(aMerged.mStatement, ledger, stages.mFifo, decisions);
    stages.mBuilt = CapitalGainsReportBuilder::build(aMerged.mStatement,
                                                     ledger,
                                                     stages.mFifo,
                                                     stages.mLoss,
                                                     {});

    return stages;
}

/// The gates after the other stages. They reject a bond or crypto instrument that is not left
/// out, so only a class that the gates themselves judge (a derivative) may be run through it.
std::vector<ProcessingDiagnostic> gatesOf(StatementMergeResult& aMerged, bool aLeaveFirstOut) {
    const auto ledger = ledgerOf(aMerged, aLeaveFirstOut);
    RunDecisions decisions;
    const auto fifo = FifoMatcher::match(aMerged.mStatement, ledger);
    const auto loss = LossEligibilityChecker::check(aMerged.mStatement, ledger, fifo, decisions);
    const auto built = CapitalGainsReportBuilder::build(aMerged.mStatement, ledger, fifo, loss, {});

    return CapitalReportGates::check(aMerged.mStatement, ledger, fifo, loss, built, {});
}

TEST(LeftOutInstrumentsTest, OnlyAnIsinInTheSetIsLeftOut) {
    ReportingYearLedger ledger;

    EXPECT_FALSE(ledger.isLeftOut("XX0000000001")); // empty set: nothing is left out

    ledger.mLeftOutIsins = {"XX0000000002"};

    EXPECT_FALSE(ledger.isLeftOut("XX0000000001"));
    EXPECT_TRUE(ledger.isLeftOut("XX0000000002"));
}

TEST(LeftOutInstrumentsTest, TheFifoPoolOfALeftOutInstrumentHasNoLotsMatchesOrDiagnostics) {
    // Bought 1 unit, sold 2: with an empty left-out set the sale has incomplete history.
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
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 2 * kOneUnit,
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto withoutFlag = FifoMatcher::match(merged.mStatement, ledgerOf(merged, false));

    ASSERT_EQ(withoutFlag.mDiagnostics.size(), 1U);
    EXPECT_EQ(withoutFlag.mDiagnostics.at(0).mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(withoutFlag.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);

    const auto withFlag = FifoMatcher::match(merged.mStatement, ledgerOf(merged, true));

    EXPECT_TRUE(withFlag.mDiagnostics.empty());
    ASSERT_EQ(withFlag.mIsins.size(), 1U);
    EXPECT_EQ(withFlag.mIsins.at(0).mStatus, FifoIsinStatus::Matched);
    EXPECT_TRUE(withFlag.mIsins.at(0).mOpenLots.empty());
    EXPECT_TRUE(withFlag.mIsins.at(0).mMatches.empty());
}

TEST(LeftOutInstrumentsTest, TheBuilderNeitherListsBlocksNorExcludesALeftOutInstrument) {
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
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR},
            },
    }};

    // The sale of 1 unit bought for EUR 10 and sold for EUR 20 is a list only with an empty
    // left-out set.
    const auto listed = runStages(merged, false);

    ASSERT_TRUE(listed.mBuilt.mReport.has_value());
    EXPECT_EQ(listed.mBuilt.mReport->mLists.size(), 1U);

    const auto leftOut = runStages(merged, true);

    ASSERT_TRUE(leftOut.mBuilt.mReport.has_value());
    EXPECT_TRUE(leftOut.mBuilt.mReport->mLists.empty());
    EXPECT_TRUE(leftOut.mBuilt.mBlocked.empty());
    EXPECT_TRUE(leftOut.mBuilt.mExcluded.empty());
}

TEST(LeftOutInstrumentsTest, ALossQuestionIsNotAskedForALeftOutInstrument) {
    // Bought for EUR 30 and sold for EUR 10 with no purchase in the 30 days around the sale: an
    // unknown class still gets the loss question.
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
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto asked = runStages(merged, false);

    EXPECT_EQ(asked.mLoss.mRequests.size(), 1U);
    EXPECT_EQ(asked.mLoss.mIsins.at(0).mSales.size(), 1U);

    const auto leftOut = runStages(merged, true);

    EXPECT_TRUE(leftOut.mLoss.mRequests.empty());
    EXPECT_TRUE(leftOut.mLoss.mIsins.at(0).mSales.empty());
}

TEST(LeftOutInstrumentsTest, ARatioQuestionIsNotAskedForALeftOutInstrument) {
    // An unconfirmed split row before the sale: an unknown class still gets the ratio question.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Unknown,
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10"),
                               .mSources = {source(2, "synthetic-buy-1")}},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(4, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 2 * kOneUnit,
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-15"),
                               .mSources = {source(3, "synthetic-split-1")}},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = kOneUnit},
            },
    }};

    RunDecisions decisionsWithoutFlag;
    const auto asked =
        matchCapitalPools(merged.mStatement, ledgerOf(merged, false), decisionsWithoutFlag);

    EXPECT_EQ(asked.mActions.mRequests.size(), 1U);

    RunDecisions decisionsWithFlag;
    const auto skipped =
        matchCapitalPools(merged.mStatement, ledgerOf(merged, true), decisionsWithFlag);

    EXPECT_TRUE(skipped.mActions.mRequests.empty());
    EXPECT_TRUE(skipped.mActions.mActions.empty());
    EXPECT_TRUE(skipped.mActions.mDiagnostics.empty());
    EXPECT_TRUE(skipped.mFifo.mDiagnostics.empty());
}

TEST(LeftOutInstrumentsTest, TheGatesGiveNoClassErrorForALeftOutInstrument) {
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
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10"),
                               .mSources = {source(3, "synthetic-sell-1")}},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = kOneUnit,
                 .mCurrency = Currency::EUR},
            },
    }};

    EXPECT_EQ(gatesOf(merged, false).size(), 1U); // unsupported_tax_treatment
    EXPECT_TRUE(gatesOf(merged, true).empty());
}

TEST(LeftOutInstrumentsTest, OnlyTheInstrumentWithTheIsinInTheSetIsSkipped) {
    // Two instruments with the same trades; only the second one (crypto) is left out.
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
                     .mUnits = kOneUnit,
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-10"),
                                   .mSources = {source(3, "synthetic-sell-1")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 2'000'000'000, // EUR 20
                     .mUnits = kOneUnit,
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Coin",
            .mIsin = "XX0000000002",
            .mAssetClass = AssetClass::Crypto,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                   .mSources = {source(4, "synthetic-buy-2")}},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = kOneUnit,
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-10"),
                                   .mSources = {source(5, "synthetic-sell-2")}},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 2'000'000'000, // EUR 20
                     .mUnits = kOneUnit,
                     .mCurrency = Currency::EUR},
                },
        },
    };

    auto ledger = ledgerOf(merged, false);

    ledger.mLeftOutIsins = {"XX0000000002"};

    RunDecisions decisions;
    const auto fifo = FifoMatcher::match(merged.mStatement, ledger);
    const auto loss = LossEligibilityChecker::check(merged.mStatement, ledger, fifo, decisions);
    const auto built = CapitalGainsReportBuilder::build(merged.mStatement, ledger, fifo, loss, {});

    ASSERT_EQ(fifo.mIsins.size(), 2U);
    EXPECT_EQ(fifo.mIsins.at(0).mMatches.size(), 1U); // the share: 1 unit bought and sold
    EXPECT_TRUE(fifo.mIsins.at(1).mMatches.empty());  // the coin is left out

    ASSERT_TRUE(built.mReport.has_value());
    EXPECT_EQ(built.mReport->mLists.size(), 1U); // only the share
    EXPECT_TRUE(built.mBlocked.empty());
    EXPECT_TRUE(built.mExcluded.empty());
}

} // namespace
