#include "processors/loss_eligibility.hpp"
#include "processors/tax_processor.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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

/// Puts all trades and actions in tax-date order. Equal dates keep the order listed in the test.
void orderChronologically(MergedStatement& aStatement) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    auto& order = aStatement.mChronologicalOrder;

    order.clear();

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

/// Prepares the real reporting-year ledger, runs the matcher and applies the loss rule without
/// any answer.
LossEligibilityResult checkYear(StatementMergeResult& aMerged,
                                int aYear,
                                OpenLossQuestions aOpenQuestions = OpenLossQuestions::Ask) {
    orderChronologically(aMerged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});
    const auto& ledger = prepared.mLedger.value();
    const auto fifo = FifoMatcher::match(aMerged.mStatement, ledger);
    RunDecisions decisions;

    return LossEligibilityChecker::check(aMerged.mStatement,
                                         ledger,
                                         fifo,
                                         decisions,
                                         aOpenQuestions);
}

/// Like checkYear, with confirmed corporate actions that the matcher applies.
LossEligibilityResult checkYearWithActions(StatementMergeResult& aMerged,
                                           int aYear,
                                           const std::vector<ConfirmedCorporateAction>& aActions) {
    orderChronologically(aMerged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});
    const auto& ledger = prepared.mLedger.value();
    const auto fifo = FifoMatcher::match(aMerged.mStatement, ledger, aActions);
    RunDecisions decisions;

    return LossEligibilityChecker::check(aMerged.mStatement, ledger, fifo, decisions);
}

// The FURS examples (*Dobiček iz kapitala – pravilo navidezne odsvojitve kapitala*, FURS, January
// 2015): every purchase at EUR 10 and every sale at EUR 8, so every sale is a loss, as FURS
// assumes. "DA-n" units keep their loss, "NE-n" units do not. The files decide the NE units; the
// open units become DA once the user answers that the investment was not traded elsewhere.

TEST(LossEligibilityTest, FursExample1) {
    // FURS example 1: DA-50; DA-50.
    // Each sale sells every unit held. The purchases before them are the sales' own purchases, so
    // no unit is replaced and both losses keep all units (DA-50, DA-50).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-12-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-05")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 5'000'000'000,   // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-07-07")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-07-14")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 5'000'000'000,   // 50 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 2U);

    // DA-50: 0 of 50 known, 50 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-01-05"),
                  .mWindowStart = day("2007-12-06"),
                  .mWindowEnd = day("2008-02-04"),
                  .mLossUnits = 5'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    // DA-50: 0 of 50 known, 50 open.
    EXPECT_EQ(sales.at(1),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 3},
                  .mSaleDate = day("2008-07-14"),
                  .mWindowStart = day("2008-06-14"),
                  .mWindowEnd = day("2008-08-13"),
                  .mLossUnits = 5'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample2) {
    // FURS example 2: DA-25; NE-50.
    // The 2007-12-12 purchase is the first sale's own purchase, not a replacement (DA-25). The
    // second sale uses 25 old units and 25 units of 2008-07-07, yet that whole purchase of 50
    // counts against it: the holding is 25 before the purchase and 25 after the sale (NE-50).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-12-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-05")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 2'500'000'000,   // 25 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-07-07")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-07-14")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 5'000'000'000,   // 50 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 2U);

    // DA-25: 0 of 25 known, 25 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-01-05"),
                  .mWindowStart = day("2007-12-06"),
                  .mWindowEnd = day("2008-02-04"),
                  .mLossUnits = 2'500'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    // NE-50: 50 of 50 known, 0 open.
    EXPECT_EQ(sales.at(1),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 3},
                  .mSaleDate = day("2008-07-14"),
                  .mWindowStart = day("2008-06-14"),
                  .mWindowEnd = day("2008-08-13"),
                  .mLossUnits = 5'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 2},
                           .mDate = day("2008-07-07"),
                           .mUnits = 5'000'000'000},
                      },
                  .mKnownIneligibleUnits = 5'000'000'000,
                  .mState = LossSaleState::Decided,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample3) {
    // FURS example 3: NE-20 DA-5; NE-50.
    // The 2008-01-31 purchase replaces 20 of the 25 units of the first sale (NE-20 DA-5). The
    // 2008-07-07 purchase of 50 counts in full against the second sale (NE-50).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-12-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-05")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 2'500'000'000,   // 25 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-31")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 2'000'000'000,     // 20 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-07-07")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-07-14")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 5'000'000'000,   // 50 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 2U);

    // NE-20 DA-5: 20 of 25 known, 5 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-01-05"),
                  .mWindowStart = day("2007-12-06"),
                  .mWindowEnd = day("2008-02-04"),
                  .mLossUnits = 2'500'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 2},
                           .mDate = day("2008-01-31"),
                           .mUnits = 2'000'000'000},
                      },
                  .mKnownIneligibleUnits = 2'000'000'000,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    // NE-50: 50 of 50 known, 0 open.
    EXPECT_EQ(sales.at(1),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 4},
                  .mSaleDate = day("2008-07-14"),
                  .mWindowStart = day("2008-06-14"),
                  .mWindowEnd = day("2008-08-13"),
                  .mLossUnits = 5'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 3},
                           .mDate = day("2008-07-07"),
                           .mUnits = 5'000'000'000},
                      },
                  .mKnownIneligibleUnits = 5'000'000'000,
                  .mState = LossSaleState::Decided,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample4) {
    // FURS example 4: NE-20 DA-80.
    // Only the 2008-05-05 purchase is within 30 days of the sale; 2008-03-05 is 41 days before it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-12-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-03-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-04-15")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 10'000'000'000,  // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-05-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 2'000'000'000,     // 20 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // NE-20 DA-80: 20 of 100 known, 80 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 4},
                  .mSaleDate = day("2008-04-15"),
                  .mWindowStart = day("2008-03-16"),
                  .mWindowEnd = day("2008-05-15"),
                  .mLossUnits = 10'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 5},
                           .mDate = day("2008-05-05"),
                           .mUnits = 2'000'000'000},
                      },
                  .mKnownIneligibleUnits = 2'000'000'000,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample5) {
    // FURS example 5: NE-100.
    // The sale uses the 2008-01-01 units; the 2008-04-01 purchase, 14 days before it, replaces all
    // 100.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-01-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-04-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-04-15")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 10'000'000'000,  // 100 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // NE-100: 100 of 100 known, 0 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mSaleDate = day("2008-04-15"),
                  .mWindowStart = day("2008-03-16"),
                  .mWindowEnd = day("2008-05-15"),
                  .mLossUnits = 10'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 1},
                           .mDate = day("2008-04-01"),
                           .mUnits = 10'000'000'000},
                      },
                  .mKnownIneligibleUnits = 10'000'000'000,
                  .mState = LossSaleState::Decided,
              }));

    EXPECT_EQ(result.mRequests.size(), 0U);
}

TEST(LossEligibilityTest, FursExample6) {
    // FURS example 6: DA-50.
    // The sale sells every unit held, and its own purchase is no replacement.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-12-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-05")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 5'000'000'000,   // 50 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // DA-50: 0 of 50 known, 50 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-01-05"),
                  .mWindowStart = day("2007-12-06"),
                  .mWindowEnd = day("2008-02-04"),
                  .mLossUnits = 5'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample7) {
    // FURS example 7: DA-25.
    // The 2007-12-12 purchase is within 30 days before the sale, but it is the sale's own purchase.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-12-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-05")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 2'500'000'000,   // 25 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // DA-25: 0 of 25 known, 25 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-01-05"),
                  .mWindowStart = day("2007-12-06"),
                  .mWindowEnd = day("2008-02-04"),
                  .mLossUnits = 2'500'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample8) {
    // FURS example 8: NE-25.
    // The 2008-01-31 purchase, 26 days after the sale, replaces all 25 units.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-12-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-05")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 2'500'000'000,   // 25 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-31")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // NE-25: 25 of 25 known, 0 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-01-05"),
                  .mWindowStart = day("2007-12-06"),
                  .mWindowEnd = day("2008-02-04"),
                  .mLossUnits = 2'500'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 2},
                           .mDate = day("2008-01-31"),
                           .mUnits = 2'500'000'000},
                      },
                  .mKnownIneligibleUnits = 2'500'000'000,
                  .mState = LossSaleState::Decided,
              }));

    EXPECT_EQ(result.mRequests.size(), 0U);
}

TEST(LossEligibilityTest, FursExample9) {
    // FURS example 9: NE-50.
    // The sale sells every unit held, but the 2008-01-31 purchase comes after it and replaces all
    // 50 units.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-12-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-05")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 5'000'000'000,   // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-31")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // NE-50: 50 of 50 known, 0 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-01-05"),
                  .mWindowStart = day("2007-12-06"),
                  .mWindowEnd = day("2008-02-04"),
                  .mLossUnits = 5'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 2},
                           .mDate = day("2008-01-31"),
                           .mUnits = 5'000'000'000},
                      },
                  .mKnownIneligibleUnits = 5'000'000'000,
                  .mState = LossSaleState::Decided,
              }));

    EXPECT_EQ(result.mRequests.size(), 0U);
}

TEST(LossEligibilityTest, FursExample10) {
    // FURS example 10: NE-10 DA-30.
    // The sale uses the 2007-02-01 units; the 2007-12-12 purchase, 24 days before it, replaces 10.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-02-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2007-12-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-05")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 4'000'000'000,   // 40 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // NE-10 DA-30: 10 of 40 known, 30 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mSaleDate = day("2008-01-05"),
                  .mWindowStart = day("2007-12-06"),
                  .mWindowEnd = day("2008-02-04"),
                  .mLossUnits = 4'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 1},
                           .mDate = day("2007-12-12"),
                           .mUnits = 1'000'000'000},
                      },
                  .mKnownIneligibleUnits = 1'000'000'000,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample11) {
    // FURS example 11: NE-100; DA-100.
    // Both sales are within 30 days of the 2008-03-20 purchase. The earlier sale takes all 100 of
    // its units, so nothing is left for the later one.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-01-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 20'000'000'000,    // 200 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 10'000'000'000,  // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-03-15")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 10'000'000'000,  // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-03-20")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 2U);

    // NE-100: 100 of 100 known, 0 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-03-10"),
                  .mWindowStart = day("2008-02-09"),
                  .mWindowEnd = day("2008-04-09"),
                  .mLossUnits = 10'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 3},
                           .mDate = day("2008-03-20"),
                           .mUnits = 10'000'000'000},
                      },
                  .mKnownIneligibleUnits = 10'000'000'000,
                  .mState = LossSaleState::Decided,
              }));

    // DA-100: 0 of 100 known, 100 open.
    EXPECT_EQ(sales.at(1),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mSaleDate = day("2008-03-15"),
                  .mWindowStart = day("2008-02-14"),
                  .mWindowEnd = day("2008-04-14"),
                  .mLossUnits = 10'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample12) {
    // FURS example 12: NE-100 DA-100.
    // The sale sells every unit held; the 2008-03-15 purchase after it replaces 100 of the 200.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-01-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 20'000'000'000,    // 200 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 20'000'000'000,  // 200 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-03-15")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // NE-100 DA-100: 100 of 200 known, 100 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-03-10"),
                  .mWindowStart = day("2008-02-09"),
                  .mWindowEnd = day("2008-04-09"),
                  .mLossUnits = 20'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 2},
                           .mDate = day("2008-03-15"),
                           .mUnits = 10'000'000'000},
                      },
                  .mKnownIneligibleUnits = 10'000'000'000,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample13) {
    // FURS example 13: NE-50.
    // The two purchases before the sale are replacements; the earliest, 2008-03-31, is taken first.
    // (FURS prints stock 10 after it; it is 100.)
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-01-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-03-31")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-04-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-04-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 5'000'000'000,   // 50 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // NE-50: 50 of 50 known, 0 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 3},
                  .mSaleDate = day("2008-04-10"),
                  .mWindowStart = day("2008-03-11"),
                  .mWindowEnd = day("2008-05-10"),
                  .mLossUnits = 5'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 1},
                           .mDate = day("2008-03-31"),
                           .mUnits = 5'000'000'000},
                      },
                  .mKnownIneligibleUnits = 5'000'000'000,
                  .mState = LossSaleState::Decided,
              }));

    EXPECT_EQ(result.mRequests.size(), 0U);
}

TEST(LossEligibilityTest, FursExample14) {
    // FURS example 14: DA-500.
    // The sale sells every unit held, so the purchases of 2008-02-04 and 2008-02-05 before it do
    // not count, although they are within 30 days.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-09-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2007-09-17")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2007-09-18")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-04")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-05")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 50'000'000'000,  // 500 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // DA-500: 0 of 500 known, 500 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 5},
                  .mSaleDate = day("2008-02-05"),
                  .mWindowStart = day("2008-01-06"),
                  .mWindowEnd = day("2008-03-06"),
                  .mLossUnits = 50'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample15) {
    // FURS example 15: NE-75 DA-25.
    // The sale uses the 2007-09-24 units; the purchases of 2007-12-18 and 2007-12-26 replace 75.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-09-24")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2007-12-18")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2007-12-26")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 2'500'000'000,     // 25 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-01-06")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 10'000'000'000,  // 100 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // NE-75 DA-25: 75 of 100 known, 25 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 3},
                  .mSaleDate = day("2008-01-06"),
                  .mWindowStart = day("2007-12-07"),
                  .mWindowEnd = day("2008-02-05"),
                  .mLossUnits = 10'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 1},
                           .mDate = day("2007-12-18"),
                           .mUnits = 5'000'000'000},
                          {.mPurchase = {StatementEventKind::Trade, 0, 2},
                           .mDate = day("2007-12-26"),
                           .mUnits = 2'500'000'000},
                      },
                  .mKnownIneligibleUnits = 7'500'000'000,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample16) {
    // FURS example 16: NE-550 DA-50; DA-350; DA-50; DA-500; DA-550.
    // The 2008-12-05 purchase replaces 550 units of the first sale. The later sales find it used
    // up; the last one sells every unit held and has only its own purchase before it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-09-24")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 60'000'000'000,    // 600 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-09-24")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 35'000'000'000,    // 350 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-09-24")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-09-24")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 50'000'000'000,    // 500 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-11-15")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 60'000'000'000,  // 600 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-11-19")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 35'000'000'000,  // 350 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-11-19")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 5'000'000'000,   // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-11-21")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 50'000'000'000,  // 500 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-12-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 55'000'000'000,    // 550 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-12-12")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 55'000'000'000,  // 550 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 5U);

    // NE-550 DA-50: 550 of 600 known, 50 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 4},
                  .mSaleDate = day("2008-11-15"),
                  .mWindowStart = day("2008-10-16"),
                  .mWindowEnd = day("2008-12-15"),
                  .mLossUnits = 60'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 8},
                           .mDate = day("2008-12-05"),
                           .mUnits = 55'000'000'000},
                      },
                  .mKnownIneligibleUnits = 55'000'000'000,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    // DA-350: 0 of 350 known, 350 open.
    EXPECT_EQ(sales.at(1),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 5},
                  .mSaleDate = day("2008-11-19"),
                  .mWindowStart = day("2008-10-20"),
                  .mWindowEnd = day("2008-12-19"),
                  .mLossUnits = 35'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    // DA-50: 0 of 50 known, 50 open.
    EXPECT_EQ(sales.at(2),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 6},
                  .mSaleDate = day("2008-11-19"),
                  .mWindowStart = day("2008-10-20"),
                  .mWindowEnd = day("2008-12-19"),
                  .mLossUnits = 5'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    // DA-500: 0 of 500 known, 500 open.
    EXPECT_EQ(sales.at(3),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 7},
                  .mSaleDate = day("2008-11-21"),
                  .mWindowStart = day("2008-10-22"),
                  .mWindowEnd = day("2008-12-21"),
                  .mLossUnits = 50'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    // DA-550: 0 of 550 known, 550 open.
    EXPECT_EQ(sales.at(4),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 9},
                  .mSaleDate = day("2008-12-12"),
                  .mWindowStart = day("2008-11-12"),
                  .mWindowEnd = day("2009-01-11"),
                  .mLossUnits = 55'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample17) {
    // FURS example 17: NE-3 DA-19; NE-2; NE-8; DA-10.
    // The first sale's own purchases are the three of 2008-05-29; only 2008-07-16 replaces (3).
    // The second sale takes 2 of 2008-07-19. The third sells every unit held, so only 2008-08-08
    // after it counts (8). 2008-08-08 is 41 days before the fourth sale.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-05-29")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-05-29")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-05-29")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-06-18")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 2'200'000'000,   // 22 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-07-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 200'000'000,     // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-07-16")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-07-19")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-08-06")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 800'000'000,     // 8 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-08-08")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-09-18")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 4U);

    // NE-3 DA-19: 3 of 22 known, 19 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 3},
                  .mSaleDate = day("2008-06-18"),
                  .mWindowStart = day("2008-05-19"),
                  .mWindowEnd = day("2008-07-18"),
                  .mLossUnits = 2'200'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 5},
                           .mDate = day("2008-07-16"),
                           .mUnits = 300'000'000},
                      },
                  .mKnownIneligibleUnits = 300'000'000,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    // NE-2: 2 of 2 known, 0 open.
    EXPECT_EQ(sales.at(1),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 4},
                  .mSaleDate = day("2008-07-10"),
                  .mWindowStart = day("2008-06-10"),
                  .mWindowEnd = day("2008-08-09"),
                  .mLossUnits = 200'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 6},
                           .mDate = day("2008-07-19"),
                           .mUnits = 200'000'000},
                      },
                  .mKnownIneligibleUnits = 200'000'000,
                  .mState = LossSaleState::Decided,
              }));

    // NE-8: 8 of 8 known, 0 open.
    EXPECT_EQ(sales.at(2),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 7},
                  .mSaleDate = day("2008-08-06"),
                  .mWindowStart = day("2008-07-07"),
                  .mWindowEnd = day("2008-09-05"),
                  .mLossUnits = 800'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 8},
                           .mDate = day("2008-08-08"),
                           .mUnits = 800'000'000},
                      },
                  .mKnownIneligibleUnits = 800'000'000,
                  .mState = LossSaleState::Decided,
              }));

    // DA-10: 0 of 10 known, 10 open.
    EXPECT_EQ(sales.at(3),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 9},
                  .mSaleDate = day("2008-09-18"),
                  .mWindowStart = day("2008-08-19"),
                  .mWindowEnd = day("2008-10-18"),
                  .mLossUnits = 1'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample18) {
    // FURS example 18: DA-1540.
    // The sale sells every unit held, so the five purchases before it do not count.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-07-25")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 12'000'000'000,    // 120 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-07-26")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 2'000'000'000,     // 20 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-08-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 40'000'000'000,    // 400 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-08-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 40'000'000'000,    // 400 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-08-08")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 60'000'000'000,    // 600 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-08-17")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 154'000'000'000, // 1540 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // DA-1540: 0 of 1540 known, 1540 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 5},
                  .mSaleDate = day("2008-08-17"),
                  .mWindowStart = day("2008-07-18"),
                  .mWindowEnd = day("2008-09-16"),
                  .mLossUnits = 154'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample19) {
    // FURS example 19: NE-100; DA-100.
    // The earlier sale takes all 100 units of the 2008-03-20 purchase; the later one finds none.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-02-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 50'000'000'000,    // 500 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 10'000'000'000,  // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-03-15")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 10'000'000'000,  // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-03-20")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 2U);

    // NE-100: 100 of 100 known, 0 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-03-10"),
                  .mWindowStart = day("2008-02-09"),
                  .mWindowEnd = day("2008-04-09"),
                  .mLossUnits = 10'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 3},
                           .mDate = day("2008-03-20"),
                           .mUnits = 10'000'000'000},
                      },
                  .mKnownIneligibleUnits = 10'000'000'000,
                  .mState = LossSaleState::Decided,
              }));

    // DA-100: 0 of 100 known, 100 open.
    EXPECT_EQ(sales.at(1),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mSaleDate = day("2008-03-15"),
                  .mWindowStart = day("2008-02-14"),
                  .mWindowEnd = day("2008-04-14"),
                  .mLossUnits = 10'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample20) {
    // FURS example 20: NE-100.
    // The sale sells every unit held; the purchases after it replace all 100 (50 + 50).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-09-24")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-03")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 10'000'000'000,  // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-11")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-13")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // NE-100: 100 of 100 known, 0 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-02-03"),
                  .mWindowStart = day("2008-01-04"),
                  .mWindowEnd = day("2008-03-04"),
                  .mLossUnits = 10'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 2},
                           .mDate = day("2008-02-10"),
                           .mUnits = 5'000'000'000},
                          {.mPurchase = {StatementEventKind::Trade, 0, 3},
                           .mDate = day("2008-02-11"),
                           .mUnits = 5'000'000'000},
                      },
                  .mKnownIneligibleUnits = 10'000'000'000,
                  .mState = LossSaleState::Decided,
              }));

    EXPECT_EQ(result.mRequests.size(), 0U);
}

TEST(LossEligibilityTest, FursExample21) {
    // FURS example 21: NE-40 DA-60.
    // The sale sells every unit held; the four purchases after it replace 40 of the 100.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2007-09-24")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-03")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 10'000'000'000,  // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-11")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-13")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // NE-40 DA-60: 40 of 100 known, 60 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-02-03"),
                  .mWindowStart = day("2008-01-04"),
                  .mWindowEnd = day("2008-03-04"),
                  .mLossUnits = 10'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 2},
                           .mDate = day("2008-02-10"),
                           .mUnits = 1'000'000'000},
                          {.mPurchase = {StatementEventKind::Trade, 0, 3},
                           .mDate = day("2008-02-11"),
                           .mUnits = 1'000'000'000},
                          {.mPurchase = {StatementEventKind::Trade, 0, 4},
                           .mDate = day("2008-02-12"),
                           .mUnits = 1'000'000'000},
                          {.mPurchase = {StatementEventKind::Trade, 0, 5},
                           .mDate = day("2008-02-13"),
                           .mUnits = 1'000'000'000},
                      },
                  .mKnownIneligibleUnits = 4'000'000'000,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample22) {
    // FURS example 22: DA-100.
    // The 2008-04-10 purchase is 67 days after the sale.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-02-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-03")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 10'000'000'000,  // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-04-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // DA-100: 0 of 100 known, 100 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-02-03"),
                  .mWindowStart = day("2008-01-04"),
                  .mWindowEnd = day("2008-03-04"),
                  .mLossUnits = 10'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, FursExample23) {
    // FURS example 23: DA-30.
    // The 2008-02-01 purchase is the sale's own purchase; 2008-04-10 is 67 days after the sale.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-02-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-02-03")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 3'000'000'000,   // 30 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2008-04-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2008);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // DA-30: 0 of 30 known, 30 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2008-02-03"),
                  .mWindowStart = day("2008-01-04"),
                  .mWindowEnd = day("2008-03-04"),
                  .mLossUnits = 3'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, DayThirtyBeforeAndAfterTheSaleCount) {
    // 2024-02-09 is 30 days before the sale of 2024-03-10, 2024-04-09 is 30 days after it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-09")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-09")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // 2 + 3 = 5 of the 10 loss units are blocked; 5 stay open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mSaleDate = day("2024-03-10"),
                  .mWindowStart = day("2024-02-09"),
                  .mWindowEnd = day("2024-04-09"),
                  .mLossUnits = 1'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 1},
                           .mDate = day("2024-02-09"),
                           .mUnits = 200'000'000},
                          {.mPurchase = {StatementEventKind::Trade, 0, 3},
                           .mDate = day("2024-04-09"),
                           .mUnits = 300'000'000},
                      },
                  .mKnownIneligibleUnits = 500'000'000,
                  .mState = LossSaleState::WaitingForAnswer,
              }));
    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, DayThirtyOneBeforeAndAfterTheSaleDoNotCount) {
    // 2024-02-08 is 31 days before the sale of 2024-03-10, 2024-04-10 is 31 days after it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-08")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mSaleDate = day("2024-03-10"),
                  .mWindowStart = day("2024-02-09"),
                  .mWindowEnd = day("2024-04-09"),
                  .mLossUnits = 1'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));
    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, AJanuaryPurchaseBlocksADecemberLossWithoutChangingTheYear) {
    // tax_rules.md: buy 100 in September 2023, sell 4 at a loss on 2024-12-20, buy 4 on
    // 2025-01-05. All 4 units are blocked, and the 2024 closing position stays unchanged.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-09-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2024-12-20"),
                  .mWindowStart = day("2024-11-20"),
                  .mWindowEnd = day("2025-01-19"),
                  .mLossUnits = 400'000'000,
                  .mReplacements = {{.mPurchase = {StatementEventKind::Trade, 0, 2},
                                     .mDate = day("2025-01-05"),
                                     .mUnits = 400'000'000}},
                  .mKnownIneligibleUnits = 400'000'000,
                  .mState = LossSaleState::Decided,
              }));
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());

    // The FIFO ledger of 2024 ends with 96 units of the 2023 purchase; the January purchase is
    // not in it.
    const auto prepared = TaxProcessor{}.prepareReportingYear({2024, merged});
    const auto fifo = FifoMatcher::match(merged.mStatement, prepared.mLedger.value());

    ASSERT_EQ(fifo.mIsins.size(), 1U);
    ASSERT_EQ(fifo.mIsins.at(0).mOpenLots.size(), 1U);

    const auto& lot = fifo.mIsins.at(0).mOpenLots.at(0);

    EXPECT_EQ(lot.mPurchaseDate, day("2023-09-01"));
    EXPECT_EQ(lot.mState.mRemainingUnits, 9'600'000'000); // 96 units
}

TEST(LossEligibilityTest, ADecember31LossCountsAJanuary30PurchaseButNotAJanuary31One) {
    // The window of a sale on 2024-12-31 ends on 2025-01-30, the last day the year's loss view
    // holds: the 2025-01-30 purchase (day 30) counts, the 2025-01-31 one (day 31) does not.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-31")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-30")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 300'000'000,     // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-31")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 200'000'000,     // 2 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // Known 3 units, open 10 - 3 = 7 units.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2024-12-31"),
                  .mWindowStart = day("2024-12-01"),
                  .mWindowEnd = day("2025-01-30"),
                  .mLossUnits = 1'000'000'000,
                  .mReplacements = {{.mPurchase = {StatementEventKind::Trade, 0, 2},
                                     .mDate = day("2025-01-30"),
                                     .mUnits = 300'000'000}},
                  .mKnownIneligibleUnits = 300'000'000,
                  .mState = LossSaleState::WaitingForAnswer,
              }));
    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& question =
        std::get<LossEligibilityQuestion>(result.mRequests.at(0).mQuestion.mData);

    ASSERT_EQ(question.mSales.size(), 1U);
    EXPECT_EQ(question.mSales.at(0).mOpenUnits, 700'000'000); // 7 units
}

TEST(LossEligibilityTest, ADecemberPurchaseCountsForAJanuaryLoss) {
    // tax_rules.md (FURS example 10 a year later): buy 50 earlier and 10 on 2023-12-12, sell 40 on
    // 2024-01-05. The December purchase blocks 10 units; 30 stay open.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-02-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 5'000'000'000,     // 50 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-12-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-01-05")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 4'000'000'000,   // 40 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mSaleDate = day("2024-01-05"),
                  .mWindowStart = day("2023-12-06"),
                  .mWindowEnd = day("2024-02-04"),
                  .mLossUnits = 4'000'000'000,
                  .mReplacements = {{.mPurchase = {StatementEventKind::Trade, 0, 1},
                                     .mDate = day("2023-12-12"),
                                     .mUnits = 1'000'000'000}},
                  .mKnownIneligibleUnits = 1'000'000'000,
                  .mState = LossSaleState::WaitingForAnswer,
              }));
}

TEST(LossEligibilityTest, AReplacementUsedByADecemberLossOfTheYearBeforeIsNotUsedAgain) {
    // The 2024-01-05 purchase is within 30 days of both losses. The earlier one, of 2023-12-20,
    // takes it, in the 2023 report and again when the 2024 report rebuilds the history.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-12-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-01-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 700'000'000, // EUR 7
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto year2023 = checkYear(merged, 2023);

    ASSERT_EQ(year2023.mIsins.size(), 1U);
    ASSERT_EQ(year2023.mIsins.at(0).mSales.size(), 1U);

    EXPECT_EQ(year2023.mIsins.at(0).mSales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2023-12-20"),
                  .mWindowStart = day("2023-11-20"),
                  .mWindowEnd = day("2024-01-19"),
                  .mLossUnits = 1'000'000'000,
                  .mReplacements = {{.mPurchase = {StatementEventKind::Trade, 0, 2},
                                     .mDate = day("2024-01-05"),
                                     .mUnits = 1'000'000'000}},
                  .mKnownIneligibleUnits = 1'000'000'000,
                  .mState = LossSaleState::Decided,
              }));

    const auto year2024 = checkYear(merged, 2024);

    ASSERT_EQ(year2024.mIsins.size(), 1U);
    ASSERT_EQ(year2024.mIsins.at(0).mSales.size(), 1U);

    // The 2024-01-10 sale finds the January purchase used up: nothing known, 10 units open.
    EXPECT_EQ(year2024.mIsins.at(0).mSales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 3},
                  .mSaleDate = day("2024-01-10"),
                  .mWindowStart = day("2023-12-11"),
                  .mWindowEnd = day("2024-02-09"),
                  .mLossUnits = 1'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));
}

TEST(LossEligibilityTest, ASaleAfterTheYearNeverTakesAReplacementFirst) {
    // The 2025-01-15 purchase is within 30 days of both losses. In the 2024 report the December
    // loss takes it and the January sale is ignored; the 2025 report gives the same answer.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-15")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto year2024 = checkYear(merged, 2024);

    ASSERT_EQ(year2024.mIsins.size(), 1U);
    ASSERT_EQ(year2024.mIsins.at(0).mSales.size(), 1U);

    EXPECT_EQ(year2024.mIsins.at(0).mSales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2024-12-20"),
                  .mWindowStart = day("2024-11-20"),
                  .mWindowEnd = day("2025-01-19"),
                  .mLossUnits = 1'000'000'000,
                  .mReplacements = {{.mPurchase = {StatementEventKind::Trade, 0, 3},
                                     .mDate = day("2025-01-15"),
                                     .mUnits = 1'000'000'000}},
                  .mKnownIneligibleUnits = 1'000'000'000,
                  .mState = LossSaleState::Decided,
              }));

    const auto year2025 = checkYear(merged, 2025);

    ASSERT_EQ(year2025.mIsins.size(), 1U);
    ASSERT_EQ(year2025.mIsins.at(0).mSales.size(), 1U);

    EXPECT_EQ(year2025.mIsins.at(0).mSales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mSaleDate = day("2025-01-10"),
                  .mWindowStart = day("2024-12-11"),
                  .mWindowEnd = day("2025-02-09"),
                  .mLossUnits = 1'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));
}

TEST(LossEligibilityTest, OnlyLossUnitsTakeReplacements) {
    // The first sale of 15 units at EUR 18 uses 10 units bought at EUR 10 (+EUR 80) and 5 bought
    // at EUR 20 (-EUR 10): only those 5 are loss units. The second sale of 5 at EUR 15 uses the
    // other 5 bought at EUR 20 (-EUR 25). If the profitable units took replacements, the first
    // sale would take all 8 units of 2024-03-20 and the second none.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-07-03")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-11")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'800'000'000, // EUR 18
                 .mUnits = 1'500'000'000,     // 15 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-20")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'800'000'000, // EUR 18
                 .mUnits = 800'000'000,       // 8 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'500'000'000, // EUR 15
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 2U);

    // 5 loss units, all 5 blocked by 2024-03-20; 3 of its 8 units are left.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mSaleDate = day("2024-03-11"),
                  .mWindowStart = day("2024-02-10"),
                  .mWindowEnd = day("2024-04-10"),
                  .mLossUnits = 500'000'000,
                  .mReplacements = {{.mPurchase = {StatementEventKind::Trade, 0, 3},
                                     .mDate = day("2024-03-20"),
                                     .mUnits = 500'000'000}},
                  .mKnownIneligibleUnits = 500'000'000,
                  .mState = LossSaleState::Decided,
              }));

    // 5 loss units, 3 blocked by the rest of 2024-03-20, 2 open.
    EXPECT_EQ(sales.at(1),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 4},
                  .mSaleDate = day("2024-04-02"),
                  .mWindowStart = day("2024-03-03"),
                  .mWindowEnd = day("2024-05-02"),
                  .mLossUnits = 500'000'000,
                  .mReplacements = {{.mPurchase = {StatementEventKind::Trade, 0, 3},
                                     .mDate = day("2024-03-20"),
                                     .mUnits = 300'000'000}},
                  .mKnownIneligibleUnits = 300'000'000,
                  .mState = LossSaleState::WaitingForAnswer,
              }));
}

TEST(LossEligibilityTest, AProfitableSaleTakesNoReplacement) {
    // Bought at EUR 10, sold at EUR 12: no loss, so the purchase 10 days later is no question.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-20")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_TRUE(result.mIsins.at(0).mSales.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(LossEligibilityTest, ALossPastTheHoldingPeriodIsNotChecked) {
    // The sale of 2025-06-02 uses 2 units of 2005 (past 15 years, -EUR 2 each, tax free) and 1
    // unit of 2020 (+EUR 3). Only units within the holding period are checked: none is a loss.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2005-04-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2020-04-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 500'000'000, // EUR 5
                 .mUnits = 200'000'000,     // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 300'000'000,     // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 100'000'000,     // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2025);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_TRUE(result.mIsins.at(0).mSales.empty());
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(LossEligibilityTest, ALossOnTheNewUnitsOfAMixedSaleIsChecked) {
    // The sale of 2025-06-02 uses 2 units of 2005 (past 15 years, +EUR 3 each, tax free) and 1
    // unit of 2020 (-EUR 2). That one loss unit is blocked by the purchase of 2025-06-10, which
    // is dated after the sale's first lot of 2005.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2005-04-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 500'000'000, // EUR 5
                 .mUnits = 200'000'000,     // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2020-04-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 300'000'000,     // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 100'000'000,     // 1 unit
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2025);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mSaleDate = day("2025-06-02"),
                  .mWindowStart = day("2025-05-03"),
                  .mWindowEnd = day("2025-07-02"),
                  .mLossUnits = 100'000'000,
                  .mReplacements = {{.mPurchase = {StatementEventKind::Trade, 0, 3},
                                     .mDate = day("2025-06-10"),
                                     .mUnits = 100'000'000}},
                  .mKnownIneligibleUnits = 100'000'000,
                  .mState = LossSaleState::Decided,
              }));
}

TEST(LossEligibilityTest, APurchaseOfAnotherIsinIsNotAReplacement) {
    // Only the same ISIN counts: the fund bought two days after the share's loss is no
    // replacement for it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share",
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-10")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-10")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 800'000'000, // EUR 8
                     .mUnits = 500'000'000,     // 5 units
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Fund",
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-03-12")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 500'000'000,       // 5 units
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 2U);
    ASSERT_EQ(result.mIsins.at(0).mSales.size(), 1U);

    EXPECT_EQ(result.mIsins.at(0).mSales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2024-03-10"),
                  .mWindowStart = day("2024-02-09"),
                  .mWindowEnd = day("2024-04-09"),
                  .mLossUnits = 500'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));
    EXPECT_TRUE(result.mIsins.at(1).mSales.empty());

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests.at(0).mQuestion.mIsin, "XX0000000001");
}

TEST(LossEligibilityTest, AForeignCurrencyPurchaseAfterTheYearCounts) {
    // The January purchase is in USD; its units count, its price is not needed.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 900'000'000, // USD 9
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::USD},
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    ASSERT_EQ(result.mIsins.at(0).mSales.size(), 1U);

    const auto& sale = result.mIsins.at(0).mSales.at(0);

    EXPECT_EQ(sale.mKnownIneligibleUnits, 400'000'000);
    EXPECT_EQ(sale.mState, LossSaleState::Decided);
}

TEST(LossEligibilityTest, ACorporateActionAfterTheYearInsideTheWindowStopsTheCheck) {
    // The split of 2025-01-10 lies inside the window of the 2024-12-20 loss (up to 2025-01-19).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-15")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 400'000'000, // EUR 4
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-10")},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 10'000'000'000}, // 100 units
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    ASSERT_EQ(result.mIsins.at(0).mSales.size(), 1U);

    // The purchase after the split is still taken, so the order of later checks stays the same.
    EXPECT_EQ(
        result.mIsins.at(0).mSales.at(0),
        (LossSaleEligibility{
            .mSale = {StatementEventKind::Trade, 0, 1},
            .mSaleDate = day("2024-12-20"),
            .mWindowStart = day("2024-11-20"),
            .mWindowEnd = day("2025-01-19"),
            .mLossUnits = 400'000'000,
            .mReplacements = {{.mPurchase = {StatementEventKind::Trade, 0, 2},
                               .mDate = day("2025-01-15"),
                               .mUnits = 400'000'000}},
            .mKnownIneligibleUnits = 400'000'000,
            .mState = LossSaleState::CorporateActionInWindow,
            .mCorporateAction = StatementEventReference{StatementEventKind::CorporateAction, 0, 0},
        }));
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(LossEligibilityTest, ACorporateActionAfterTheWindowDoesNotStopTheCheck) {
    // The split of 2025-01-20 is one day after the window of the 2024-12-20 loss.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2025-01-20")},
                 .mType = CorporateActionType::UnresolvedSplit,
                 .mUnitsDelta = 10'000'000'000}, // 100 units
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    ASSERT_EQ(result.mIsins.at(0).mSales.size(), 1U);

    const auto& sale = result.mIsins.at(0).mSales.at(0);

    EXPECT_EQ(sale.mState, LossSaleState::Decided);
    EXPECT_EQ(sale.mKnownIneligibleUnits, 400'000'000);
    EXPECT_FALSE(sale.mCorporateAction.has_value());
}

TEST(LossEligibilityTest, OneQuestionDescribesEveryOpenSaleOfTheIsin) {
    // The first sale is the day-30 scenario (5 of 10 units blocked, 5 open). The second sale, of
    // 5 units on 2024-06-10, has no purchase within 30 days (5 open).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2023-06-01"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 10'000'000'000,    // 100 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-02-09"),
                                       .mSources = {source(3, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 200'000'000,       // 2 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-03-10"),
                                       .mSources = {source(4, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 800'000'000, // EUR 8
                         .mUnits = 1'000'000'000,   // 10 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-04-09"),
                                       .mSources = {source(5, "synthetic-buy-3")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 300'000'000,       // 3 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-06-10"),
                                       .mSources = {source(6, "synthetic-sell-2")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 800'000'000, // EUR 8
                         .mUnits = 500'000'000,     // 5 units
                         .mCurrency = Currency::EUR},
                    },
            }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& request = result.mRequests.at(0);
    const auto& question = request.mQuestion;

    EXPECT_EQ(question.mType, RequestType::LossEligibilityConfirmationRequired);
    EXPECT_EQ(question.mAffectedReports, (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(question.mSources,
              (std::vector<SourceReference>{source(4, "synthetic-sell-1"),
                                            source(6, "synthetic-sell-2")}));
    EXPECT_EQ(question.mTaxDate, day("2024-03-10"));
    EXPECT_EQ(question.mIsin, "XX0000000001");
    EXPECT_EQ(request.mProcessingDiagnostics, (std::vector<std::size_t>{0, 1}));

    ASSERT_TRUE(std::holds_alternative<LossEligibilityQuestion>(question.mData));
    EXPECT_EQ(
        std::get<LossEligibilityQuestion>(question.mData),
        (LossEligibilityQuestion{
            .mIsin = "XX0000000001",
            .mName = "Synthetic Share",
            .mSales =
                {
                    {.mSaleDate = day("2024-03-10"),
                     .mSaleTransaction = TransactionIdentity{.mBroker = Broker::TradeRepublic,
                                                             .mTransactionId = "synthetic-sell-1"},
                     .mSaleUnits = 1'000'000'000, // 10 units
                     .mLossUnits = 1'000'000'000,
                     .mWindowStart = day("2024-02-09"),
                     .mWindowEnd = day("2024-04-09"),
                     .mKnownReplacements =
                         {
                             {.mDate = day("2024-02-09"), .mUnits = 200'000'000},
                             {.mDate = day("2024-04-09"), .mUnits = 300'000'000},
                         },
                     .mKnownIneligibleUnits = 500'000'000,
                     .mOpenUnits = 500'000'000},
                    {.mSaleDate = day("2024-06-10"),
                     .mSaleTransaction = TransactionIdentity{.mBroker = Broker::TradeRepublic,
                                                             .mTransactionId = "synthetic-sell-2"},
                     .mSaleUnits = 500'000'000, // 5 units
                     .mLossUnits = 500'000'000,
                     .mWindowStart = day("2024-05-11"),
                     .mWindowEnd = day("2024-07-10"),
                     .mKnownIneligibleUnits = 0,
                     .mOpenUnits = 500'000'000},
                },
        }));

    // One error per open sale, linked to the question; no units or amounts in the message.
    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& first = result.mDiagnostics.at(0);

    EXPECT_EQ(first.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(first.mCode, ProcessingDiagnosticCode::LossEligibilityUnresolved);
    EXPECT_EQ(first.mMessageKey, MessageKey::LossEligibilityUnresolvedConfirmationRequired);
    EXPECT_TRUE(first.mMessageParams.empty());
    EXPECT_EQ(first.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(first.mContext.mAffectedReports, (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(first.mContext.mSources,
              (std::vector<SourceReference>{source(4, "synthetic-sell-1")}));
    EXPECT_EQ(first.mContext.mTaxDate, day("2024-03-10"));
    EXPECT_EQ(first.mContext.mInstrumentName, "Synthetic Share");
    EXPECT_EQ(first.mContext.mIsin, "XX0000000001");

    const auto& second = result.mDiagnostics.at(1);

    EXPECT_EQ(second.mMessageKey, MessageKey::LossEligibilityUnresolvedConfirmationRequired);
    EXPECT_EQ(second.mContext.mSources,
              (std::vector<SourceReference>{source(6, "synthetic-sell-2")}));
    EXPECT_EQ(second.mContext.mTaxDate, day("2024-06-10"));
}

TEST(LossEligibilityTest, ARebuyLaterOnTheDayOfTheSaleAndItsPurchaseCounts) {
    // One day, in this order: buy 10, sell all 10 at a loss, buy 10 again. The rebuy is dated on
    // the sale's original date, but it comes after the sale, so it is not the sale's own purchase.
    // The holding is 10 before the sale and 10 after the rebuy: NE-10.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // NE-10: 10 of 10 known, 0 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2024-03-01"),
                  .mWindowStart = day("2024-01-31"),
                  .mWindowEnd = day("2024-03-31"),
                  .mLossUnits = 1'000'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 2},
                           .mDate = day("2024-03-01"),
                           .mUnits = 1'000'000'000},
                      },
                  .mKnownIneligibleUnits = 1'000'000'000,
                  .mState = LossSaleState::Decided,
              }));
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(LossEligibilityTest, ARebuyOfPartLaterOnTheSameDayCounts) {
    // One day, in this order: buy 10, sell 5 at a loss, buy 5 again. The holding is 10 before the
    // sale and 10 after the rebuy: NE-5.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 500'000'000,     // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 500'000'000,     // 5 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // NE-5: 5 of 5 known, 0 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mSaleDate = day("2024-03-01"),
                  .mWindowStart = day("2024-01-31"),
                  .mWindowEnd = day("2024-03-31"),
                  .mLossUnits = 500'000'000,
                  .mReplacements =
                      {
                          {.mPurchase = {StatementEventKind::Trade, 0, 2},
                           .mDate = day("2024-03-01"),
                           .mUnits = 500'000'000},
                      },
                  .mKnownIneligibleUnits = 500'000'000,
                  .mState = LossSaleState::Decided,
              }));
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(LossEligibilityTest, APurchaseEarlierOnTheDayOfTheSaleIsItsOwn) {
    // One day, in this order: buy 10, buy 10, sell 10 at a loss. Both purchases come before the
    // sale on its original date, so both are the sale's own purchases, as the three same-day
    // purchases of FURS example 17: no unit is known to be replaced.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& sales = result.mIsins.at(0).mSales;

    ASSERT_EQ(sales.size(), 1U);

    // 0 of 10 known, 10 open.
    EXPECT_EQ(sales.at(0),
              (LossSaleEligibility{
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mSaleDate = day("2024-03-01"),
                  .mWindowStart = day("2024-01-31"),
                  .mWindowEnd = day("2024-03-31"),
                  .mLossUnits = 1'000'000'000,
                  .mKnownIneligibleUnits = 0,
                  .mState = LossSaleState::WaitingForAnswer,
              }));
    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(LossEligibilityTest, AStoppedPoolIsSkipped) {
    // A sale without enough units stops the pool; the report builder blocks the ISIN, so the loss
    // rule asks nothing about it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 500'000'000,     // 5 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_EQ(result.mIsins.at(0).mIsin, "XX0000000001");
    EXPECT_EQ(result.mIsins.at(0).mName, "Synthetic Share");
    EXPECT_TRUE(result.mIsins.at(0).mSales.empty());
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(LossEligibilityTest, ALossOfAShareOrAFundIsChecked) {
    // Each investment: bought 10 at EUR 10, sold 5 at EUR 8 (a loss on 5 units), bought 2 ten
    // days later. Shares and funds can reach Doh-KDVP, so the 2 units are known and 3 stay open.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share",
            .mIsin = "XX0000000001",
            .mAssetClass = AssetClass::Stock,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-10")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-10")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 800'000'000, // EUR 8
                     .mUnits = 500'000'000,     // 5 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-20")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Fund",
            .mIsin = "XX0000000002",
            .mAssetClass = AssetClass::Fund,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-10")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-10")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 800'000'000, // EUR 8
                     .mUnits = 500'000'000,     // 5 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-20")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 2U);

    ASSERT_EQ(result.mIsins.at(0).mSales.size(), 1U);

    const auto& sale0 = result.mIsins.at(0).mSales.at(0);

    EXPECT_EQ(result.mIsins.at(0).mIsin, "XX0000000001");
    EXPECT_EQ(sale0.mLossUnits, 500'000'000);            // 5 units
    EXPECT_EQ(sale0.mKnownIneligibleUnits, 200'000'000); // 2 units of 2024-03-20
    EXPECT_EQ(sale0.mState, LossSaleState::WaitingForAnswer);

    ASSERT_EQ(result.mIsins.at(1).mSales.size(), 1U);

    const auto& sale1 = result.mIsins.at(1).mSales.at(0);

    EXPECT_EQ(result.mIsins.at(1).mIsin, "XX0000000002");
    EXPECT_EQ(sale1.mLossUnits, 500'000'000);            // 5 units
    EXPECT_EQ(sale1.mKnownIneligibleUnits, 200'000'000); // 2 units of 2024-03-20
    EXPECT_EQ(sale1.mState, LossSaleState::WaitingForAnswer);

    EXPECT_EQ(result.mRequests.size(), 2U); // one question per ISIN
}

TEST(LossEligibilityTest, ALossOfAnInvestmentOutsideDohKdvpIsNotChecked) {
    // The same trades as for a share, but bonds, derivatives, crypto and private funds never
    // reach Doh-KDVP: the gates block them, so the loss rule asks nothing about them.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Bond",
            .mIsin = "XX0000000001",
            .mAssetClass = AssetClass::Bond,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-10")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-10")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 800'000'000, // EUR 8
                     .mUnits = 500'000'000,     // 5 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-20")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Derivative",
            .mIsin = "XX0000000002",
            .mAssetClass = AssetClass::Derivative,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-10")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-10")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 800'000'000, // EUR 8
                     .mUnits = 500'000'000,     // 5 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-20")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Coin",
            .mIsin = "XX0000000003",
            .mAssetClass = AssetClass::Crypto,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-10")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-10")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 800'000'000, // EUR 8
                     .mUnits = 500'000'000,     // 5 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-20")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Private Fund",
            .mIsin = "XX0000000004",
            .mAssetClass = AssetClass::PrivateFund,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-10")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-10")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 800'000'000, // EUR 8
                     .mUnits = 500'000'000,     // 5 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-20")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = checkYear(merged, 2024);

    ASSERT_EQ(result.mIsins.size(), 4U);
    EXPECT_TRUE(result.mIsins.at(0).mSales.empty());
    EXPECT_TRUE(result.mIsins.at(1).mSales.empty());
    EXPECT_TRUE(result.mIsins.at(2).mSales.empty());
    EXPECT_TRUE(result.mIsins.at(3).mSales.empty());

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(LossEligibilityTest, AssumeBlockedTreatsTheOpenUnitsAsBlockedWithOneWarning) {
    // Bought 10 at EUR 10. The sale of 5 at EUR 8 is a loss; the purchase of 2 on 2024-03-20
    // blocks 2 units, 3 stay open. The sale of 3 at EUR 8 is a loss with no purchase within 30
    // days: 3 open. Without a way to ask, every open unit is assumed blocked.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments =
        {
            {
                .mName = "Synthetic Share",
                .mIsin = "XX0000000001",
                .mTransactions =
                    {
                        {.mMetadata = {.mTaxDate = day("2024-01-10"),
                                       .mSources = {source(2, "synthetic-buy-1")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 1'000'000'000,     // 10 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-03-10"),
                                       .mSources = {source(3, "synthetic-sell-1")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 800'000'000, // EUR 8
                         .mUnits = 500'000'000,     // 5 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-03-20"),
                                       .mSources = {source(4, "synthetic-buy-2")}},
                         .mTradeSide = TradeSide::Buy,
                         .mUnitPrice = 1'000'000'000, // EUR 10
                         .mUnits = 200'000'000,       // 2 units
                         .mCurrency = Currency::EUR},
                        {.mMetadata = {.mTaxDate = day("2024-06-10"),
                                       .mSources = {source(5, "synthetic-sell-2")}},
                         .mTradeSide = TradeSide::Sell,
                         .mUnitPrice = 800'000'000, // EUR 8
                         .mUnits = 300'000'000,     // 3 units
                         .mCurrency = Currency::EUR},
                    },
            }};

    const auto result = checkYear(merged, 2024, OpenLossQuestions::AssumeBlocked);

    ASSERT_EQ(result.mIsins.size(), 1U);
    ASSERT_EQ(result.mIsins.at(0).mSales.size(), 2U);

    const auto& first = result.mIsins.at(0).mSales.at(0);

    EXPECT_EQ(first.mKnownIneligibleUnits, 200'000'000);    // 2 units of 2024-03-20
    EXPECT_EQ(first.mDeclaredIneligibleUnits, 300'000'000); // 5 - 2 = 3 open units
    EXPECT_EQ(first.ineligibleUnits(), 500'000'000);        // all 5 loss units
    EXPECT_TRUE(first.mAssumedBlocked);
    EXPECT_EQ(first.mState, LossSaleState::Decided);

    const auto& second = result.mIsins.at(0).mSales.at(1);

    EXPECT_EQ(second.mKnownIneligibleUnits, 0);
    EXPECT_EQ(second.mDeclaredIneligibleUnits, 300'000'000); // 3 open units
    EXPECT_TRUE(second.mAssumedBlocked);
    EXPECT_EQ(second.mState, LossSaleState::Decided);

    EXPECT_TRUE(result.mRequests.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& warning = result.mDiagnostics.at(0);

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, ProcessingDiagnosticCode::LossEligibilityUnresolved);
    EXPECT_EQ(warning.mMessageKey, MessageKey::LossEligibilityUnresolvedAssumedBlocked);
    EXPECT_TRUE(warning.mMessageParams.empty());
    EXPECT_EQ(warning.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(warning.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(warning.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(warning.mContext.mInstrumentName, "Synthetic Share");
    EXPECT_EQ(warning.mContext.mTaxDate, day("2024-03-10")); // the first open sale
    EXPECT_EQ(warning.mContext.mSources,
              (std::vector<SourceReference>{source(3, "synthetic-sell-1"),
                                            source(5, "synthetic-sell-2")}));
}

TEST(LossEligibilityTest, AssumeBlockedLeavesALossTheFilesDecideAlone) {
    // tax_rules.md: buy 100 in September 2023, sell 4 at a loss on 2024-12-20, buy 4 on
    // 2025-01-05. The January purchase blocks all 4 loss units, so nothing is assumed.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-09-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 10'000'000'000,    // 100 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = checkYear(merged, 2024, OpenLossQuestions::AssumeBlocked);

    ASSERT_EQ(result.mIsins.size(), 1U);
    ASSERT_EQ(result.mIsins.at(0).mSales.size(), 1U);

    const auto& sale = result.mIsins.at(0).mSales.at(0);

    EXPECT_EQ(sale.mKnownIneligibleUnits, 400'000'000); // 4 units of 2025-01-05
    EXPECT_EQ(sale.mDeclaredIneligibleUnits, 0);
    EXPECT_FALSE(sale.mAssumedBlocked);
    EXPECT_EQ(sale.mState, LossSaleState::Decided);

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(LossEligibilityTest, AnAppliedSplitInsideTheWindowStopsTheCheck) {
    // 4 units bought, 3 sold at a loss on 2024-11-20 (window up to 2024-12-20), split 2 / 1
    // applied on 2024-12-01: the trade units before and after it cannot be compared yet.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-11-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 300'000'000,     // 3 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-12-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        checkYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-12-01")}});

    ASSERT_EQ(result.mIsins.size(), 1U);
    ASSERT_EQ(result.mIsins.at(0).mSales.size(), 1U);

    const auto& sale = result.mIsins.at(0).mSales.at(0);

    EXPECT_EQ(sale.mWindowEnd, day("2024-12-20"));
    EXPECT_EQ(sale.mLossUnits, 300'000'000); // 3 units
    EXPECT_EQ(sale.mState, LossSaleState::AppliedCorporateActionBefore);
    EXPECT_EQ(sale.mCorporateAction,
              (StatementEventReference{StatementEventKind::CorporateAction, 0, 0}));
    // No question: the answer could not be used yet.
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(LossEligibilityTest, AnAppliedSplitBeforeTheWindowStopsTheCheck) {
    // 1 unit bought in 2023, split 2 / 1 on 2023-06-01, the 2 units sold on 2023-09-01; 3 bought
    // and sold at a loss in January 2024. The split is long before the window, but the stock the
    // rule counts in trade units (1 - 2 + 3 - 3 = -1) mixes units before and after it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-09-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-01-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 300'000'000,     // 3 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        checkYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2023-06-01")}});

    ASSERT_EQ(result.mIsins.size(), 1U);
    ASSERT_EQ(result.mIsins.at(0).mSales.size(), 1U);
    EXPECT_EQ(result.mIsins.at(0).mSales.at(0).mState, LossSaleState::AppliedCorporateActionBefore);
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(LossEligibilityTest, AnAppliedSplitAfterTheWindowDoesNotStopTheCheck) {
    // The loss sale of 2024-03-01 has its window up to 2024-03-31; the split of 2024-06-01 is
    // after it, so the rule counts as before: no replacement, 5 open units, a question.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 500'000'000,     // 5 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        checkYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-06-01")}});

    ASSERT_EQ(result.mIsins.size(), 1U);
    ASSERT_EQ(result.mIsins.at(0).mSales.size(), 1U);

    const auto& sale = result.mIsins.at(0).mSales.at(0);

    EXPECT_EQ(sale.mWindowEnd, day("2024-03-31"));
    EXPECT_EQ(sale.mState, LossSaleState::WaitingForAnswer);
    EXPECT_FALSE(sale.mCorporateAction.has_value());
    EXPECT_EQ(result.mRequests.size(), 1U);
}

} // namespace
