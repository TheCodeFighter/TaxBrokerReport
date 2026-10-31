#include "processors/capital_gains_report_builder.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string_view>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
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

/// Prepares the real reporting-year ledger, runs the matcher and the loss rule (no answers) and
/// builds the report from them.
CapitalGainsBuildResult
buildYear(StatementMergeResult& aMerged, int aYear, const std::vector<Isin>& aExcludedIsins = {}) {
    orderChronologically(aMerged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});
    const auto fifo = FifoMatcher{}.match(aMerged.mStatement, prepared.mLedger.value());
    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(aMerged.mStatement,
                                                    prepared.mLedger.value(),
                                                    fifo,
                                                    decisions);

    return CapitalGainsReportBuilder{}.build(aMerged.mStatement,
                                             prepared.mLedger.value(),
                                             fifo,
                                             loss,
                                             aExcludedIsins);
}

/// Like buildYear, with confirmed corporate actions that the matcher applies.
CapitalGainsBuildResult
buildYearWithActions(StatementMergeResult& aMerged,
                     int aYear,
                     const std::vector<ConfirmedCorporateAction>& aActions) {
    orderChronologically(aMerged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({aYear, aMerged});
    const auto fifo = FifoMatcher::match(aMerged.mStatement, prepared.mLedger.value(), aActions);
    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(aMerged.mStatement,
                                                    prepared.mLedger.value(),
                                                    fifo,
                                                    decisions);

    return CapitalGainsReportBuilder{}.build(aMerged.mStatement,
                                             prepared.mLedger.value(),
                                             fifo,
                                             loss,
                                             {});
}

const KdvpPurchaseRow& purchaseAt(const KdvpInventoryList& aList, std::size_t aRow) {
    return std::get<KdvpPurchaseRow>(aList.mRows.at(aRow).mEntry);
}

const KdvpSaleRow& saleAt(const KdvpInventoryList& aList, std::size_t aRow) {
    return std::get<KdvpSaleRow>(aList.mRows.at(aRow).mEntry);
}

/// Euro in ticks of 10^-16 EUR, the scale of gainsOfRows (8 decimals of units times 8 of price).
WideInteger eurTicks(std::int64_t aEuro) {
    return WideInteger{aEuro} * WideInteger{10'000'000'000'000'000};
}

/// An independent FIFO over the rows of a list: purchase rows queue their F3 units at F4, each
/// sale row takes F7 units from the oldest rows. Returns the gain of every sale row in ticks of
/// 10^-16 EUR. It uses no production code, only the rows.
std::vector<WideInteger> gainsOfRows(const KdvpInventoryList& aList) {
    struct OpenUnits {
        Units mUnits;
        UnitPrice mUnitPrice;
    };

    std::deque<OpenUnits> queue;
    std::vector<WideInteger> gains;

    for (const auto& row : aList.mRows)
    {
        if (const auto* purchase = std::get_if<KdvpPurchaseRow>(&row.mEntry))
        {
            queue.push_back({purchase->mUnits, purchase->mUnitPrice});
            continue;
        }

        const auto& sale = std::get<KdvpSaleRow>(row.mEntry);
        Units left = sale.mUnits;
        WideInteger gain = 0;

        while (left > 0)
        {
            if (queue.empty())
            {
                ADD_FAILURE() << "a sale row sells more units than the purchase rows list";
                break;
            }

            const Units taken = std::min(left, queue.front().mUnits);

            gain += WideInteger{taken} * (sale.mUnitPrice - queue.front().mUnitPrice);
            queue.front().mUnits -= taken;
            left -= taken;

            if (queue.front().mUnits == 0)
            {
                queue.pop_front();
            }
        }

        gains.push_back(gain);
    }

    return gains;
}

TEST(CapitalGainsReportBuilderTest, ReportMetadataDefaultsToAnOriginalReturnOfAResident) {
    const ReportMetadata metadata;

    EXPECT_EQ(metadata.mReportingYear, 0);
    EXPECT_EQ(metadata.mFilingType, FilingType::Original);
    EXPECT_TRUE(metadata.mTaxpayer.mIsResident);
    EXPECT_TRUE(metadata.mTaxpayer.mTaxNumber.empty());
    EXPECT_FALSE(metadata.mTaxpayer.mName.has_value());
    EXPECT_FALSE(metadata.mTaxpayer.mAddress.has_value());
    EXPECT_FALSE(metadata.mTaxpayer.mCity.has_value());
    EXPECT_FALSE(metadata.mTaxpayer.mPostCode.has_value());
    EXPECT_FALSE(metadata.mTaxpayer.mPostName.has_value());
    EXPECT_FALSE(metadata.mTaxpayer.mBirthDate.has_value());
    EXPECT_FALSE(metadata.mTaxpayer.mEmail.has_value());
    EXPECT_FALSE(metadata.mTaxpayer.mPhoneNumber.has_value());
}

TEST(CapitalGainsReportBuilderTest, SaleUsingTwoPurchasesIsOneSaleRowAndTwoPurchaseRows) {
    // Example "one sale uses two purchases" in tax_rules.md.
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
                {.mMetadata = {.mTaxDate = day("2024-02-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 2'000'000'000,     // 20 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 1'500'000'000,     // 15 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    EXPECT_EQ(list.mIsin, "XX0000000001");
    EXPECT_EQ(list.mName, "Synthetic Share");
    EXPECT_FALSE(list.mIsFund);
    ASSERT_EQ(list.mRows.size(), 3U);

    // Row 1: 10 units of the first purchase, F8 = 10.
    const auto& firstPurchase = purchaseAt(list, 0);

    EXPECT_EQ(firstPurchase.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(firstPurchase.mDate, day("2024-01-10"));
    EXPECT_EQ(firstPurchase.mMethod, KdvpPurchaseMethod::B);
    EXPECT_EQ(firstPurchase.mUnits, 1'000'000'000);     // 10 units
    EXPECT_EQ(firstPurchase.mUnitPrice, 1'000'000'000); // EUR 10
    EXPECT_EQ(list.mRows.at(0).mStock, 1'000'000'000);  // 10

    // Row 2: only 5 of the 20 units of the second purchase are used, F8 = 10 + 5.
    const auto& secondPurchase = purchaseAt(list, 1);

    EXPECT_EQ(secondPurchase.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(secondPurchase.mDate, day("2024-02-10"));
    EXPECT_EQ(secondPurchase.mMethod, KdvpPurchaseMethod::B);
    EXPECT_EQ(secondPurchase.mUnits, 500'000'000);       // 5 units
    EXPECT_EQ(secondPurchase.mUnitPrice, 2'000'000'000); // EUR 20
    EXPECT_EQ(list.mRows.at(1).mStock, 1'500'000'000);   // 15

    // Row 3: one sale row of 15 units, not two rows of 10 and 5. F8 = 15 - 15.
    const auto& sale = saleAt(list, 2);

    EXPECT_EQ(sale.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(sale.mDate, day("2024-03-10"));
    EXPECT_EQ(sale.mUnits, 1'500'000'000);     // 15 units
    EXPECT_EQ(sale.mUnitPrice, 3'000'000'000); // EUR 30
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(2).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest, PurchasePartlySoldInEarlierYearListsOnlyTheEarlierSale2023) {
    // Example "a purchase partly sold in an earlier year" in tax_rules.md, year 2023. The 2024
    // sale is after the year and not in the ledger.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'500'000'000,     // 15 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-05-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'400'000'000, // EUR 14
                 .mUnits = 600'000'000,       // 6 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2023);

    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    // Only the 4 units that the 2023 sale uses, not the 15 units bought.
    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(purchase.mDate, day("2022-03-01"));
    EXPECT_EQ(purchase.mUnits, 400'000'000);       // 4 units
    EXPECT_EQ(purchase.mUnitPrice, 1'000'000'000); // EUR 10
    EXPECT_EQ(list.mRows.at(0).mStock, 400'000'000);

    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(sale.mDate, day("2023-05-10"));
    EXPECT_EQ(sale.mUnits, 400'000'000);       // 4 units
    EXPECT_EQ(sale.mUnitPrice, 1'200'000'000); // EUR 12
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest, EarlierYearSaleIsNotListedAndItsUnitsAreLeftOut2024) {
    // The same history as above, year 2024: the 2023 sale used 4 units, so 11 remain and the 2024
    // sale of 6 units uses 6 of them.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'500'000'000,     // 15 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-05-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'400'000'000, // EUR 14
                 .mUnits = 600'000'000,       // 6 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(purchase.mDate, day("2022-03-01"));
    EXPECT_EQ(purchase.mUnits, 600'000'000);       // 6 units
    EXPECT_EQ(purchase.mUnitPrice, 1'000'000'000); // EUR 10
    EXPECT_EQ(list.mRows.at(0).mStock, 600'000'000);

    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(sale.mDate, day("2024-06-20"));
    EXPECT_EQ(sale.mUnits, 600'000'000);       // 6 units
    EXPECT_EQ(sale.mUnitPrice, 1'400'000'000); // EUR 14
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest, StockLeftAfterTheYearIsNotListed2024) {
    // Example "stock left after the year" in tax_rules.md, year 2024. The purchase of
    // 2024-02-01 is one row of 10 units (4 + 6) although two sales use it. The sale of 2025 is
    // not in the 2024 ledger.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-05-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-08-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'100'000'000, // EUR 11
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-10-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'300'000'000, // EUR 13
                 .mUnits = 800'000'000,       // 8 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-15")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'500'000'000, // EUR 15
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 4U);

    // Row 1: purchase of 10 units, F8 = 10.
    const auto& firstPurchase = purchaseAt(list, 0);

    EXPECT_EQ(firstPurchase.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(firstPurchase.mDate, day("2024-02-01"));
    EXPECT_EQ(firstPurchase.mUnits, 1'000'000'000);     // 4 + 6 = 10 units
    EXPECT_EQ(firstPurchase.mUnitPrice, 1'000'000'000); // EUR 10
    EXPECT_EQ(list.mRows.at(0).mStock, 1'000'000'000);

    // Row 2: sale of 4 units, F8 = 10 - 4 = 6.
    const auto& firstSale = saleAt(list, 1);

    EXPECT_EQ(firstSale.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(firstSale.mDate, day("2024-05-01"));
    EXPECT_EQ(firstSale.mUnits, 400'000'000);       // 4 units
    EXPECT_EQ(firstSale.mUnitPrice, 1'200'000'000); // EUR 12
    EXPECT_EQ(firstSale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 600'000'000);

    // Row 3: only 2 of the 5 units of the second purchase are used, F8 = 6 + 2 = 8.
    const auto& secondPurchase = purchaseAt(list, 2);

    EXPECT_EQ(secondPurchase.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(secondPurchase.mDate, day("2024-08-01"));
    EXPECT_EQ(secondPurchase.mUnits, 200'000'000);       // 2 units
    EXPECT_EQ(secondPurchase.mUnitPrice, 1'100'000'000); // EUR 11
    EXPECT_EQ(list.mRows.at(2).mStock, 800'000'000);

    // Row 4: sale of 8 units, F8 = 8 - 8 = 0. The 3 units still held are not listed.
    const auto& secondSale = saleAt(list, 3);

    EXPECT_EQ(secondSale.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 3}));
    EXPECT_EQ(secondSale.mDate, day("2024-10-01"));
    EXPECT_EQ(secondSale.mUnits, 800'000'000);       // 8 units
    EXPECT_EQ(secondSale.mUnitPrice, 1'300'000'000); // EUR 13
    EXPECT_EQ(secondSale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(3).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest, StockLeftAfterTheYearIsListedInTheNextYear2025) {
    // The same history as above, year 2025: the 3 units left from the purchase of 2024-08-01.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-05-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-08-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'100'000'000, // EUR 11
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-10-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'300'000'000, // EUR 13
                 .mUnits = 800'000'000,       // 8 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-15")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'500'000'000, // EUR 15
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2025);

    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    // Row 1: the 3 units left from the purchase of 2024-08-01, F8 = 3.
    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(purchase.mDate, day("2024-08-01"));
    EXPECT_EQ(purchase.mUnits, 300'000'000);       // 3 units
    EXPECT_EQ(purchase.mUnitPrice, 1'100'000'000); // EUR 11
    EXPECT_EQ(list.mRows.at(0).mStock, 300'000'000);

    // Row 2: sale of 3 units, F8 = 3 - 3 = 0.
    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 4}));
    EXPECT_EQ(sale.mDate, day("2025-01-15"));
    EXPECT_EQ(sale.mUnits, 300'000'000);       // 3 units
    EXPECT_EQ(sale.mUnitPrice, 1'500'000'000); // EUR 15
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest, YearWithoutASaleGivesAReportWithoutLists) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-06-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'100'000'000, // EUR 11
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    ASSERT_TRUE(result.mReport.has_value());
    EXPECT_TRUE(result.mReport->mLists.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    EXPECT_TRUE(result.mBlocked.empty());
}

TEST(CapitalGainsReportBuilderTest,
     ListsFollowThePresentationOrderAndInstrumentsWithoutASaleHaveNone) {
    // The builder keeps the order of the presentation. The merger sorts it by ISIN, so the lists
    // are in ISIN order in a real run.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share A",
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Share B",
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units, never sold
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Share C",
            .mIsin = "XX0000000003",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = buildYear(merged, 2024);

    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 2U);

    const auto& first = result.mReport->mLists.at(0);

    EXPECT_EQ(first.mIsin, "XX0000000001");
    EXPECT_EQ(first.mName, "Synthetic Share A");
    ASSERT_EQ(first.mRows.size(), 2U);

    const auto& firstPurchase = purchaseAt(first, 0);
    const auto& firstSale = saleAt(first, 1);

    EXPECT_EQ(firstPurchase.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(firstSale.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 1}));

    const auto& second = result.mReport->mLists.at(1);

    EXPECT_EQ(second.mIsin, "XX0000000003");
    EXPECT_EQ(second.mName, "Synthetic Share C");
    ASSERT_EQ(second.mRows.size(), 2U);

    const auto& secondPurchase = purchaseAt(second, 0);
    const auto& secondSale = saleAt(second, 1);

    EXPECT_EQ(secondPurchase.mPurchase, (StatementEventReference{StatementEventKind::Trade, 2, 0}));
    EXPECT_EQ(secondSale.mSale, (StatementEventReference{StatementEventKind::Trade, 2, 1}));
}

TEST(CapitalGainsReportBuilderTest, UnitsAndUnitValuesKeepEightDecimals) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'234'567'891, // EUR 12.34567891
                 .mUnits = 150'000'000,       // 1.5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'300'000'001, // EUR 13.00000001
                 .mUnits = 12'345'678,        // 0.12345678 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    // Only the 0.12345678 units that the sale uses, at the purchase unit value.
    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mUnits, 12'345'678);        // 0.12345678 units
    EXPECT_EQ(purchase.mUnitPrice, 1'234'567'891); // EUR 12.34567891
    EXPECT_EQ(list.mRows.at(0).mStock, 12'345'678);

    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mUnits, 12'345'678);        // 0.12345678 units
    EXPECT_EQ(sale.mUnitPrice, 1'300'000'001); // EUR 13.00000001
    EXPECT_EQ(list.mRows.at(1).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest, SaleWithZeroGainIsNotALossAndIsListed) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'000'000'000, // EUR 10, gain 5 * (10 - 10) = 0
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);
    ASSERT_EQ(result.mReport->mLists.at(0).mRows.size(), 2U);
    EXPECT_EQ(saleAt(result.mReport->mLists.at(0), 1).mLossRule, KdvpLossRule::NotBlocked);
}

TEST(CapitalGainsReportBuilderTest, PurchaseOnTheSaleDayIsListedAtItsLedgerPosition) {
    // The sale and the second purchase have the same tax date; the test lists the sale first, so
    // the ledger keeps that order and the second purchase row comes after the sale row.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'100'000'000, // EUR 11
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'300'000'000, // EUR 13
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 4U);

    // Row 1: purchase of 5 units, F8 = 5.
    const auto& firstPurchase = purchaseAt(list, 0);

    EXPECT_EQ(firstPurchase.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 0}));
    EXPECT_EQ(list.mRows.at(0).mStock, 500'000'000);

    // Row 2: sale of 5 units, F8 = 5 - 5 = 0.
    const auto& firstSale = saleAt(list, 1);

    EXPECT_EQ(firstSale.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(list.mRows.at(1).mStock, 0);

    // Row 3: the purchase of the sale day comes after the sale row, F8 = 0 + 5 = 5.
    const auto& secondPurchase = purchaseAt(list, 2);

    EXPECT_EQ(secondPurchase.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 2}));
    EXPECT_EQ(list.mRows.at(2).mStock, 500'000'000);

    // Row 4: sale of 5 units, F8 = 5 - 5 = 0.
    const auto& secondSale = saleAt(list, 3);

    EXPECT_EQ(secondSale.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 3}));
    EXPECT_EQ(list.mRows.at(3).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest,
     AnOpenLossQuestionOnALossMakingPortionOfAProfitableSaleBlocksTheIsin) {
    // Sale of 15 units at EUR 18: 10 units from the EUR 10 purchase give +EUR 80, 5 units from
    // the EUR 20 purchase give -EUR 10. The sale gains EUR 70 but has a loss-making portion. No
    // purchase lies within 30 days of the sale, so the loss question is open and nothing is
    // written for the sale.
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
                {.mMetadata = {.mTaxDate = day("2024-01-20")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'800'000'000, // EUR 18
                 .mUnits = 1'500'000'000,     // 15 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_FALSE(result.mReport.has_value());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_EQ(result.mBlocked.size(), 1U);
    EXPECT_EQ(result.mBlocked.at(0),
              (KdvpBlockedIsin{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mReason = KdvpBlockReason::LossNotReportable,
                  .mSales = {{StatementEventKind::Trade, 0, 2}},
              }));
}

TEST(CapitalGainsReportBuilderTest, OnlySalesWithALossMakingPortionAreListedAsBlocked) {
    // Sale 1 at EUR 8 and sale 3 at EUR 9 are below the EUR 10 purchase; sale 2 at EUR 12 is not.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 200'000'000,     // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 900'000'000, // EUR 9
                 .mUnits = 200'000'000,     // 2 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_FALSE(result.mReport.has_value());
    ASSERT_EQ(result.mBlocked.size(), 1U);
    EXPECT_EQ(result.mBlocked.at(0).mReason, KdvpBlockReason::LossNotReportable);
    EXPECT_EQ(result.mBlocked.at(0).mSales,
              (std::vector<StatementEventReference>{{StatementEventKind::Trade, 0, 1},
                                                    {StatementEventKind::Trade, 0, 3}}));
}

TEST(CapitalGainsReportBuilderTest, BlockedIsinMeansNoReportEvenWhenAnotherIsinIsFine) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share A",
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Share B",
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 800'000'000, // EUR 8, a loss
                     .mUnits = 200'000'000,     // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = buildYear(merged, 2024);

    EXPECT_FALSE(result.mReport.has_value());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_EQ(result.mBlocked.size(), 1U);
    EXPECT_EQ(result.mBlocked.at(0).mIsin, "XX0000000002");
    EXPECT_EQ(result.mBlocked.at(0).mName, "Synthetic Share B");
    EXPECT_EQ(result.mBlocked.at(0).mReason, KdvpBlockReason::LossNotReportable);
    EXPECT_EQ(result.mBlocked.at(0).mSales,
              (std::vector<StatementEventReference>{{StatementEventKind::Trade, 1, 1}}));
}

TEST(CapitalGainsReportBuilderTest, StoppedPoolsBlockWithAllSalesOfTheYear) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            // A: a split on 2024-02-01 stops the pool before the sale.
            .mName = "Synthetic Share A",
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 500'000'000,       // 5 units
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mType = CorporateActionType::Split,
                     .mRatio = SplitRatio{.mNewShares = 2, .mOldShares = 1}},
                },
        },
        {
            // B: the sale is in USD, which the matcher does not convert yet.
            .mName = "Synthetic Share B",
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // USD 12
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::USD},
                },
        },
        {
            // C: the first sale needs 2 units but only 1 was bought; the second sale is ignored
            // by the stopped pool but is still a sale of the year.
            .mName = "Synthetic Share C",
            .mIsin = "XX0000000003",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-04-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 100'000'000,       // 1 unit
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = buildYear(merged, 2024);

    EXPECT_FALSE(result.mReport.has_value());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_EQ(result.mBlocked.size(), 3U);

    EXPECT_EQ(result.mBlocked.at(0),
              (KdvpBlockedIsin{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share A",
                  .mReason = KdvpBlockReason::FifoStopped,
                  .mSales = {{StatementEventKind::Trade, 0, 1}},
              }));
    EXPECT_EQ(result.mBlocked.at(1),
              (KdvpBlockedIsin{
                  .mIsin = "XX0000000002",
                  .mName = "Synthetic Share B",
                  .mReason = KdvpBlockReason::FifoStopped,
                  .mSales = {{StatementEventKind::Trade, 1, 1}},
              }));
    EXPECT_EQ(result.mBlocked.at(2),
              (KdvpBlockedIsin{
                  .mIsin = "XX0000000003",
                  .mName = "Synthetic Share C",
                  .mReason = KdvpBlockReason::FifoStopped,
                  .mSales = {{StatementEventKind::Trade, 2, 1}, {StatementEventKind::Trade, 2, 2}},
              }));
}

TEST(CapitalGainsReportBuilderTest, StoppedPoolWithoutASaleInTheYearDoesNotBlock) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            // A: the split of 2023 stops the pool, and A has no sale in 2024.
            .mName = "Synthetic Share A",
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions =
                {
                    {.mMetadata = {.mTaxDate = day("2023-06-01")},
                     .mType = CorporateActionType::Split,
                     .mRatio = SplitRatio{.mNewShares = 2, .mOldShares = 1}},
                },
        },
        {
            .mName = "Synthetic Share B",
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);
    EXPECT_EQ(result.mReport->mLists.at(0).mIsin, "XX0000000002");
}

TEST(CapitalGainsReportBuilderTest, StoppedPoolIsReportedBeforeALossMadeBeforeTheStop) {
    // The sale of 2 units at EUR 8 is a loss and is matched before the split of 2024-03-01 stops
    // the pool. The reason is the stopped pool, not the loss.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 200'000'000,     // 2 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mType = CorporateActionType::Split,
                 .mRatio = SplitRatio{.mNewShares = 2, .mOldShares = 1}},
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_FALSE(result.mReport.has_value());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_EQ(result.mBlocked.size(), 1U);
    EXPECT_EQ(result.mBlocked.at(0),
              (KdvpBlockedIsin{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mReason = KdvpBlockReason::FifoStopped,
                  .mSales = {{StatementEventKind::Trade, 0, 1}},
              }));
}

TEST(CapitalGainsReportBuilderTest, AssetClassDoesNotChangeTheListAndIsFundStaysFalse) {
    // The class check is the classification gate's job; the builder ignores the class.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Fund",
            .mIsin = "XX0000000001",
            .mAssetClass = AssetClass::Fund,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Unknown",
            .mIsin = "XX0000000002",
            .mAssetClass = AssetClass::Unknown,
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 2U);
    EXPECT_EQ(result.mReport->mLists.at(0).mIsin, "XX0000000001");
    EXPECT_FALSE(result.mReport->mLists.at(0).mIsFund);
    EXPECT_EQ(result.mReport->mLists.at(1).mIsin, "XX0000000002");
    EXPECT_FALSE(result.mReport->mLists.at(1).mIsFund);
}

TEST(CapitalGainsReportBuilderTest, AgreedExclusionLeavesOutTheBlockedIsinAndReportsTheRest) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share A",
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Share B",
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 800'000'000, // EUR 8, a loss
                     .mUnits = 200'000'000,     // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = buildYear(merged, 2024, {"XX0000000002"});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);
    EXPECT_EQ(result.mReport->mLists.at(0).mIsin, "XX0000000001");
    ASSERT_EQ(result.mReport->mLists.at(0).mRows.size(), 2U);
    EXPECT_EQ(saleAt(result.mReport->mLists.at(0), 1).mUnitPrice, 1'200'000'000); // EUR 12

    ASSERT_EQ(result.mExcluded.size(), 1U);
    EXPECT_EQ(result.mExcluded.at(0),
              (KdvpExcludedIsin{
                  .mIsin = "XX0000000002",
                  .mName = "Synthetic Share B",
                  .mBlockReason = KdvpBlockReason::LossNotReportable,
                  .mSales = {{StatementEventKind::Trade, 1, 1}},
              }));
}

TEST(CapitalGainsReportBuilderTest, AnExcludedIsinWithoutABlockGetsNoRows) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share A",
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Share B",
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'300'000'000, // EUR 13
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = buildYear(merged, 2024, {"XX0000000002"});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);
    EXPECT_EQ(result.mReport->mLists.at(0).mIsin, "XX0000000001");
    EXPECT_TRUE(result.mLongHeld.empty());

    // B has no block (buy 2 at EUR 10, sell 2 at EUR 13, a gain), but the user left it out.
    ASSERT_EQ(result.mExcluded.size(), 1U);
    EXPECT_EQ(result.mExcluded.at(0),
              (KdvpExcludedIsin{
                  .mIsin = "XX0000000002",
                  .mName = "Synthetic Share B",
                  .mBlockReason = std::nullopt,
                  .mSales = {{StatementEventKind::Trade, 1, 1}},
              }));
}

TEST(CapitalGainsReportBuilderTest, LeavingOutEveryBlockedIsinGivesAnEmptyReport) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share B",
        .mIsin = "XX0000000002",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8, a loss
                 .mUnits = 200'000'000,     // 2 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024, {"XX0000000002"});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    EXPECT_TRUE(result.mReport->mLists.empty());
    ASSERT_EQ(result.mExcluded.size(), 1U);
    EXPECT_EQ(result.mExcluded.at(0).mIsin, "XX0000000002");
}

TEST(CapitalGainsReportBuilderTest, PartlyAgreedExclusionStillBlocksTheOtherIsins) {
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            // A: stopped by a split, and the user agrees to leave it out.
            .mName = "Synthetic Share A",
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 1'000'000'000,     // 10 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 500'000'000,       // 5 units
                     .mCurrency = Currency::EUR},
                },
            .mCorporateActions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mType = CorporateActionType::Split,
                     .mRatio = SplitRatio{.mNewShares = 2, .mOldShares = 1}},
                },
        },
        {
            // B: a loss; the user has not agreed to leave it out.
            .mName = "Synthetic Share B",
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 800'000'000, // EUR 8
                     .mUnits = 200'000'000,     // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = buildYear(merged, 2024, {"XX0000000001"});

    EXPECT_FALSE(result.mReport.has_value());
    ASSERT_EQ(result.mExcluded.size(), 1U);
    EXPECT_EQ(result.mExcluded.at(0).mIsin, "XX0000000001");
    EXPECT_EQ(result.mExcluded.at(0).mBlockReason, KdvpBlockReason::FifoStopped);
    ASSERT_EQ(result.mBlocked.size(), 1U);
    EXPECT_EQ(result.mBlocked.at(0).mIsin, "XX0000000002");
    EXPECT_EQ(result.mBlocked.at(0).mReason, KdvpBlockReason::LossNotReportable);
}

TEST(CapitalGainsReportBuilderTest, LossFromAnEarlierYearDoesNotBlockTheSelectedYear2024) {
    // The 2023 sale at EUR 8 is a loss of EUR 8 (4 x (8 - 10)). A loss counts only in its own year
    // and is not carried forward, so the 2024 list has only the 2024 sale and is not blocked.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-06-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 600'000'000,       // 6 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    // Row 1: the 6 units left after the 2023 sale, F8 = 6.
    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mDate, day("2023-01-10"));
    EXPECT_EQ(purchase.mUnits, 600'000'000);       // 6 units
    EXPECT_EQ(purchase.mUnitPrice, 1'000'000'000); // EUR 10
    EXPECT_EQ(list.mRows.at(0).mStock, 600'000'000);

    // Row 2: sale of 6 units, F8 = 6 - 6 = 0.
    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mDate, day("2024-03-01"));
    EXPECT_EQ(sale.mUnits, 600'000'000);       // 6 units
    EXPECT_EQ(sale.mUnitPrice, 1'200'000'000); // EUR 12
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest, UnitsPastFifteenYearsAreTaxFreeAndNotListed2024) {
    // Bought 2009-03-10: the 15 years end on 2024-03-10, so a sale on 2024-03-11 is past them.
    // The ISIN gets no list and is not blocked; the units stay visible in mLongHeld.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2009-03-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-11")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_TRUE(result.mReport.has_value());
    EXPECT_TRUE(result.mReport->mLists.empty());
    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mUnits = 1'000'000'000, // 10 units
              }));
}

TEST(CapitalGainsReportBuilderTest, SaleOnTheFifteenthAnniversaryIsStillTaxed2024) {
    // Bought 2009-03-10: a sale on 2024-03-10, the day the 15 years end, is still inside them.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2009-03-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    EXPECT_TRUE(result.mLongHeld.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    // Row 1: purchase of 10 units, F8 = 10.
    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mDate, day("2009-03-10"));
    EXPECT_EQ(purchase.mUnits, 1'000'000'000);     // 10 units
    EXPECT_EQ(purchase.mUnitPrice, 1'000'000'000); // EUR 10
    EXPECT_EQ(list.mRows.at(0).mStock, 1'000'000'000);

    // Row 2: sale of 10 units, F8 = 10 - 10 = 0.
    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mDate, day("2024-03-10"));
    EXPECT_EQ(sale.mUnits, 1'000'000'000);     // 10 units
    EXPECT_EQ(sale.mUnitPrice, 3'000'000'000); // EUR 30
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest, SaleOfTwoOldUnitsAndOneNewUnitListsOnlyTheNewUnit2025) {
    // FIFO: the sale of 3 units takes the 2 units from 2005 (20 years, past the 15) and 1 unit
    // from 2020. The 2 old units are tax free, so the list has 1 purchase unit and 1 sale unit.
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
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2025);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    // Row 1: 1 of the 2 units bought 2020-04-01, F8 = 1.
    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mPurchase, (StatementEventReference{StatementEventKind::Trade, 0, 1}));
    EXPECT_EQ(purchase.mDate, day("2020-04-01"));
    EXPECT_EQ(purchase.mUnits, 100'000'000);       // 1 unit
    EXPECT_EQ(purchase.mUnitPrice, 2'000'000'000); // EUR 20
    EXPECT_EQ(list.mRows.at(0).mStock, 100'000'000);

    // Row 2: 1 of the 3 sold units, F8 = 1 - 1 = 0.
    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mDate, day("2025-06-02"));
    EXPECT_EQ(sale.mUnits, 100'000'000);       // 1 unit
    EXPECT_EQ(sale.mUnitPrice, 3'000'000'000); // EUR 30
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);

    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mUnits = 200'000'000, // 2 units
              }));
}

TEST(CapitalGainsReportBuilderTest, SaleUsingOldAndNewUnitsListsOnlyTheNewUnits2024) {
    // FIFO takes 12 units from the 2008 purchase (past 15 years) and 8 from the 2023 purchase.
    // Only the 8 new units are listed, in the purchase row and in the sale row.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-05-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'200'000'000,     // 12 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-09-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 1'600'000'000,     // 16 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 2'000'000'000,     // 20 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    // Row 1: 8 of the 16 units bought 2023-09-01, F8 = 8.
    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mDate, day("2023-09-01"));
    EXPECT_EQ(purchase.mUnits, 800'000'000);       // 8 units
    EXPECT_EQ(purchase.mUnitPrice, 2'000'000'000); // EUR 20
    EXPECT_EQ(list.mRows.at(0).mStock, 800'000'000);

    // Row 2: 8 of the 20 sold units, F8 = 8 - 8 = 0.
    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mDate, day("2024-06-20"));
    EXPECT_EQ(sale.mUnits, 800'000'000);       // 8 units
    EXPECT_EQ(sale.mUnitPrice, 3'000'000'000); // EUR 30
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);

    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mUnits = 1'200'000'000, // 12 units
              }));
}

TEST(CapitalGainsReportBuilderTest, SaleWithOnlyOldUnitsHasNoRowAndALaterSaleIsListed2024) {
    // FIFO: the 2024-03-01 sale uses the 10 units from 2008 (past 15 years) and gets no row; the
    // 2024-06-01 sale uses 5 units from 2023 and is listed.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    // Row 1: 5 of the 10 units bought 2023-01-10, F8 = 5.
    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mDate, day("2023-01-10"));
    EXPECT_EQ(purchase.mUnits, 500'000'000);       // 5 units
    EXPECT_EQ(purchase.mUnitPrice, 2'000'000'000); // EUR 20
    EXPECT_EQ(list.mRows.at(0).mStock, 500'000'000);

    // Row 2: the 2024-06-01 sale of 5 units, F8 = 5 - 5 = 0.
    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mSale, (StatementEventReference{StatementEventKind::Trade, 0, 3}));
    EXPECT_EQ(sale.mDate, day("2024-06-01"));
    EXPECT_EQ(sale.mUnits, 500'000'000);       // 5 units
    EXPECT_EQ(sale.mUnitPrice, 3'000'000'000); // EUR 30
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);

    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mUnits = 1'000'000'000, // 10 units
              }));
}

TEST(CapitalGainsReportBuilderTest, SaleIn2021UsesTheTwentyYearRule) {
    // A sale before 2022 uses 20 years: bought 2004-05-01, sold 2021-06-01 is 17 years.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2004-05-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2021-06-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2021);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    EXPECT_TRUE(result.mLongHeld.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    // Row 1: purchase of 10 units, F8 = 10.
    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mDate, day("2004-05-01"));
    EXPECT_EQ(purchase.mUnits, 1'000'000'000);     // 10 units
    EXPECT_EQ(purchase.mUnitPrice, 1'000'000'000); // EUR 10
    EXPECT_EQ(list.mRows.at(0).mStock, 1'000'000'000);

    // Row 2: sale of 10 units, F8 = 10 - 10 = 0.
    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mDate, day("2021-06-01"));
    EXPECT_EQ(sale.mUnits, 1'000'000'000);     // 10 units
    EXPECT_EQ(sale.mUnitPrice, 3'000'000'000); // EUR 30
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest, SaleIn2021AfterTwentyYearsIsNotListed) {
    // Bought 2000-05-01, sold 2021-06-01 is 21 years, past the 20 years of a sale before 2022.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2000-05-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2021-06-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2021);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_TRUE(result.mReport.has_value());
    EXPECT_TRUE(result.mReport->mLists.empty());
    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mUnits = 1'000'000'000, // 10 units
              }));
}

TEST(CapitalGainsReportBuilderTest, SaleIn2022UsesTheFifteenYearRule) {
    // The same purchase as in the 2021 test, sold 2022-01-03: 17 years is past the 15 years that
    // apply to a sale from 2022. The sale date decides, not the purchase date.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2004-05-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2022-01-03")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2022);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_TRUE(result.mReport.has_value());
    EXPECT_TRUE(result.mReport->mLists.empty());
    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mUnits = 1'000'000'000, // 10 units
              }));
}

TEST(CapitalGainsReportBuilderTest, PurchaseOnLeapDayEndsOnTheTwentyEighthAndIsStillTaxed) {
    // Bought 2008-02-29: 2023 has no 29 February, so the 15 years end on 2023-02-28.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-02-29")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-02-28")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2023);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    EXPECT_TRUE(result.mLongHeld.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    // Row 1: purchase of 10 units, F8 = 10.
    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mDate, day("2008-02-29"));
    EXPECT_EQ(purchase.mUnits, 1'000'000'000);     // 10 units
    EXPECT_EQ(purchase.mUnitPrice, 1'000'000'000); // EUR 10
    EXPECT_EQ(list.mRows.at(0).mStock, 1'000'000'000);

    // Row 2: sale of 10 units, F8 = 10 - 10 = 0.
    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mDate, day("2023-02-28"));
    EXPECT_EQ(sale.mUnits, 1'000'000'000);     // 10 units
    EXPECT_EQ(sale.mUnitPrice, 3'000'000'000); // EUR 30
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest, PurchaseOnLeapDayIsPastTheFifteenYearsOnFirstOfMarch) {
    // Bought 2008-02-29: the 15 years end on 2023-02-28, so 2023-03-01 is past them.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-02-29")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2023);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_TRUE(result.mReport.has_value());
    EXPECT_TRUE(result.mReport->mLists.empty());
    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mUnits = 1'000'000'000, // 10 units
              }));
}

TEST(CapitalGainsReportBuilderTest, LossPastTheHoldingPeriodDoesNotBlock2024) {
    // The sale at EUR 8 is a loss, but the units are past 15 years: the loss cannot reduce the
    // tax base, so it is left out with the units and does not block.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 800'000'000, // EUR 8
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_TRUE(result.mReport.has_value());
    EXPECT_TRUE(result.mReport->mLists.empty());
    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 1},
                  .mUnits = 1'000'000'000, // 10 units
              }));
}

TEST(CapitalGainsReportBuilderTest, LossOnlyOnTheOldUnitsOfAMixedSaleDoesNotBlock2025) {
    // FIFO: 2 units from 2005 at EUR 20 (past 15 years, loss 2 x (15 - 20) = EUR -10) and 1 unit
    // from 2020 at EUR 10 (gain 15 - 10 = EUR 5). Only the listed unit is checked for a loss.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2005-04-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2020-04-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'500'000'000, // EUR 15
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2025);

    EXPECT_TRUE(result.mBlocked.empty());
    EXPECT_TRUE(result.mExcluded.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    // Row 1: 1 of the 2 units bought 2020-04-01, F8 = 1.
    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mDate, day("2020-04-01"));
    EXPECT_EQ(purchase.mUnits, 100'000'000);       // 1 unit
    EXPECT_EQ(purchase.mUnitPrice, 1'000'000'000); // EUR 10
    EXPECT_EQ(list.mRows.at(0).mStock, 100'000'000);

    // Row 2: 1 of the 3 sold units, F8 = 1 - 1 = 0.
    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mDate, day("2025-06-02"));
    EXPECT_EQ(sale.mUnits, 100'000'000);       // 1 unit
    EXPECT_EQ(sale.mUnitPrice, 1'500'000'000); // EUR 15
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);

    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0),
              (KdvpLongHeldUnits{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mSale = {StatementEventKind::Trade, 0, 2},
                  .mUnits = 200'000'000, // 2 units
              }));
}

TEST(CapitalGainsReportBuilderTest, LossOnTheNewUnitsOfAMixedSaleBlocks2025) {
    // FIFO: 2 units from 2005 at EUR 10 (past 15 years) and 1 unit from 2020 at EUR 20, sold at
    // EUR 15: the listed unit has a loss of 15 - 20 = EUR -5, so the loss rule is needed.
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
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'500'000'000, // EUR 15
                 .mUnits = 300'000'000,       // 3 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2025);

    EXPECT_FALSE(result.mReport.has_value());
    EXPECT_TRUE(result.mExcluded.empty());
    EXPECT_TRUE(result.mLongHeld.empty());
    ASSERT_EQ(result.mBlocked.size(), 1U);
    EXPECT_EQ(result.mBlocked.at(0),
              (KdvpBlockedIsin{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mReason = KdvpBlockReason::LossNotReportable,
                  .mSales = {{StatementEventKind::Trade, 0, 2}},
              }));
}

TEST(CapitalGainsReportBuilderTest, ALossBlockedInFullIsListedWithF10False2024) {
    // tax_rules.md: buy 100 in September 2023, sell 4 at a loss on 2024-12-20, buy 4 on
    // 2025-01-05. The January purchase blocks the loss of all 4 units: F10 false, no question.
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

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    // Row 1: 4 units of the 2023 purchase, F8 = 4. The January purchase has no row.
    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mDate, day("2023-09-01"));
    EXPECT_EQ(purchase.mUnits, 400'000'000);         // 4 units
    EXPECT_EQ(list.mRows.at(0).mStock, 400'000'000); // 4

    // Row 2: the sale of 4 units with F10 false, F8 = 0.
    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mDate, day("2024-12-20"));
    EXPECT_EQ(sale.mUnits, 400'000'000);     // 4 units
    EXPECT_EQ(sale.mUnitPrice, 800'000'000); // EUR 8
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::Blocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest, ALossOnTheNewUnitsIsListedWithF10False2025) {
    // The sale of 2025-06-02 uses 2 units of 2005 (past 15 years, tax free, not listed) and 1
    // unit of 2020 (-EUR 2). The purchase of 2025-06-10 blocks the loss of that listed unit.
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

    const auto result = buildYear(merged, 2025);

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);

    const auto& purchase = purchaseAt(list, 0);

    EXPECT_EQ(purchase.mDate, day("2020-04-01"));
    EXPECT_EQ(purchase.mUnits, 100'000'000);         // 1 unit
    EXPECT_EQ(list.mRows.at(0).mStock, 100'000'000); // 1

    const auto& sale = saleAt(list, 1);

    EXPECT_EQ(sale.mUnits, 100'000'000); // 1 unit
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::Blocked);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);

    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0).mUnits, 200'000'000); // 2 units
}

TEST(CapitalGainsReportBuilderTest, ABlockedLossAfterProfitableUnitsIsTwoSaleRows2024) {
    // The sale of 15 units at EUR 18 uses 10 units bought at EUR 10 (+EUR 80) and 5 bought at
    // EUR 20 (-EUR 10). The 2024-03-20 purchase of 8 units blocks the loss of all 5 loss units, so
    // 5 of the 15 listed units are blocked: the sale is two rows, the profitable units first.
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
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 4U);
    EXPECT_EQ(list.mRows.at(0),
              (KdvpRow{.mEntry = KdvpPurchaseRow{.mPurchase = {StatementEventKind::Trade, 0, 0},
                                                 .mDate = day("2023-06-01"),
                                                 .mMethod = KdvpPurchaseMethod::B,
                                                 .mUnits = 1'000'000'000,      // 10 units
                                                 .mUnitPrice = 1'000'000'000}, // EUR 10
                       .mStock = 1'000'000'000}));                             // 10
    EXPECT_EQ(list.mRows.at(1),
              (KdvpRow{.mEntry = KdvpPurchaseRow{.mPurchase = {StatementEventKind::Trade, 0, 1},
                                                 .mDate = day("2023-07-03"),
                                                 .mMethod = KdvpPurchaseMethod::B,
                                                 .mUnits = 500'000'000,        // 5 units
                                                 .mUnitPrice = 2'000'000'000}, // EUR 20
                       .mStock = 1'500'000'000}));                             // 15
    // The 10 units from the EUR 10 purchase are not blocked (F10 true); F8 = 15 - 10.
    EXPECT_EQ(list.mRows.at(2),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 2},
                                             .mDate = day("2024-03-11"),
                                             .mUnits = 1'000'000'000,     // 10 units
                                             .mUnitPrice = 1'800'000'000, // EUR 18
                                             .mLossRule = KdvpLossRule::NotBlocked},
                       .mStock = 500'000'000})); // 5
    // The 5 units from the EUR 20 purchase are the blocked loss (F10 false); F8 = 5 - 5.
    EXPECT_EQ(list.mRows.at(3),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 2},
                                             .mDate = day("2024-03-11"),
                                             .mUnits = 500'000'000,       // 5 units
                                             .mUnitPrice = 1'800'000'000, // EUR 18
                                             .mLossRule = KdvpLossRule::Blocked},
                       .mStock = 0}));

    // 10 x (18 - 10) = +EUR 80 and 5 x (18 - 20) = -EUR 10.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{eurTicks(80), eurTicks(-10)}));
}

TEST(CapitalGainsReportBuilderTest, ABlockedLossBeforeProfitableUnitsIsTwoSaleRows2024) {
    // The sale of 15 units at EUR 18 uses 10 units bought at EUR 20 (-EUR 20) and 5 bought at
    // EUR 10 (+EUR 40). The 2024-03-20 purchase of 10 units blocks the loss of the 10 loss units.
    // The blocked units come first because FIFO sells them first.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-07-03")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
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
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2024);

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 4U);
    EXPECT_EQ(list.mRows.at(0).mStock, 1'000'000'000); // purchase of 10 units, F8 = 10
    EXPECT_EQ(list.mRows.at(1).mStock, 1'500'000'000); // purchase of 5 units, F8 = 15
    EXPECT_EQ(list.mRows.at(2),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 2},
                                             .mDate = day("2024-03-11"),
                                             .mUnits = 1'000'000'000,     // 10 units
                                             .mUnitPrice = 1'800'000'000, // EUR 18
                                             .mLossRule = KdvpLossRule::Blocked},
                       .mStock = 500'000'000})); // F8 = 15 - 10
    EXPECT_EQ(list.mRows.at(3),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 2},
                                             .mDate = day("2024-03-11"),
                                             .mUnits = 500'000'000,       // 5 units
                                             .mUnitPrice = 1'800'000'000, // EUR 18
                                             .mLossRule = KdvpLossRule::NotBlocked},
                       .mStock = 0}));

    // 10 x (18 - 20) = -EUR 20 and 5 x (18 - 10) = +EUR 40.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{eurTicks(-20), eurTicks(40)}));
}

TEST(CapitalGainsReportBuilderTest, ABlockedLossOfASplitLotNextToProfitableUnitsIsTwoSaleRows2024) {
    // Lot A: 10 units at EUR 100 on 2023-05-02. Lot B: 10 units at EUR 20 on 2023-06-03. The 2 / 1
    // split on 2024-02-01 makes A 20 units at EUR 50 and B 20 units at EUR 10. The sale of 30
    // units at EUR 40 uses all of A (-EUR 10 a unit, 20 loss units) and 10 of B (+EUR 30 a unit).
    // The purchase of 20 units on 2024-03-20 blocks the 20 loss units.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-05-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 1'000'000'000,      // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-06-03")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-11")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 4'000'000'000, // EUR 40
                 .mUnits = 3'000'000'000,     // 30 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-20")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 4'000'000'000, // EUR 40
                 .mUnits = 2'000'000'000,     // 20 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-02-01")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 4U);
    EXPECT_EQ(purchaseAt(list, 0).mUnits, 2'000'000'000);     // 20 units of lot A, F3
    EXPECT_EQ(purchaseAt(list, 0).mUnitPrice, 5'000'000'000); // EUR 100 / 2 = EUR 50, F4
    EXPECT_EQ(list.mRows.at(0).mStock, 2'000'000'000);        // F8 = 20
    EXPECT_EQ(purchaseAt(list, 1).mUnits, 1'000'000'000);     // 10 of the 20 units of lot B
    EXPECT_EQ(purchaseAt(list, 1).mUnitPrice, 1'000'000'000); // EUR 20 / 2 = EUR 10, F4
    EXPECT_EQ(list.mRows.at(1).mStock, 3'000'000'000);        // F8 = 30
    EXPECT_EQ(saleAt(list, 2).mUnits, 2'000'000'000);         // 20 units, F7
    EXPECT_EQ(saleAt(list, 2).mLossRule, KdvpLossRule::Blocked);
    EXPECT_EQ(list.mRows.at(2).mStock, 1'000'000'000); // F8 = 30 - 20
    EXPECT_EQ(saleAt(list, 3).mUnits, 1'000'000'000);  // 10 units, F7
    EXPECT_EQ(saleAt(list, 3).mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(3).mStock, 0);

    // 20 x (40 - 50) = -EUR 200 and 10 x (40 - 10) = +EUR 300.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{eurTicks(-200), eurTicks(300)}));
}

TEST(CapitalGainsReportBuilderTest, ABlockedLossNextToLongHeldAndProfitableUnitsIsTwoSaleRows2025) {
    // The sale of 6 units at EUR 40 on 2025-06-02 uses 2 units of 2005 (past 15 years, not listed,
    // their loss is not checked), 2 units of 2020 at EUR 30 (+EUR 20) and 2 units of 2021 at
    // EUR 50 (-EUR 20). The purchase of 2 units on 2025-06-10 blocks the 2 listed loss units.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2005-04-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 5'000'000'000, // EUR 50
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2020-04-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2021-05-03")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 5'000'000'000, // EUR 50
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 4'000'000'000, // EUR 40
                 .mUnits = 600'000'000,       // 6 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-06-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 4'000'000'000, // EUR 40
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
            },
    }};

    const auto result = buildYear(merged, 2025);

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 4U);
    EXPECT_EQ(purchaseAt(list, 0).mDate, day("2020-04-01"));
    EXPECT_EQ(purchaseAt(list, 0).mUnits, 200'000'000); // 2 units
    EXPECT_EQ(list.mRows.at(0).mStock, 200'000'000);    // F8 = 2
    EXPECT_EQ(purchaseAt(list, 1).mDate, day("2021-05-03"));
    EXPECT_EQ(purchaseAt(list, 1).mUnits, 200'000'000); // 2 units
    EXPECT_EQ(list.mRows.at(1).mStock, 400'000'000);    // F8 = 4
    EXPECT_EQ(saleAt(list, 2).mUnits, 200'000'000);     // 2 units of 2020, F7
    EXPECT_EQ(saleAt(list, 2).mLossRule, KdvpLossRule::NotBlocked);
    EXPECT_EQ(list.mRows.at(2).mStock, 200'000'000); // F8 = 4 - 2
    EXPECT_EQ(saleAt(list, 3).mUnits, 200'000'000);  // 2 units of 2021, F7
    EXPECT_EQ(saleAt(list, 3).mLossRule, KdvpLossRule::Blocked);
    EXPECT_EQ(list.mRows.at(3).mStock, 0);

    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0).mUnits, 200'000'000); // the 2 units of 2005

    // 2 x (40 - 30) = +EUR 20 and 2 x (40 - 50) = -EUR 20.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{eurTicks(20), eurTicks(-20)}));
}

TEST(CapitalGainsReportBuilderTest, AnExcludedLongHeldIsinKeepsNoUnitsInTheLongHeldList) {
    // A is bought 2008-01-10 and sold 2024-03-10: past 15 years, so it is not blocked and the
    // user's exclusion does nothing. B is bought and sold in 2024 and is listed.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {
        {
            .mName = "Synthetic Share A",
            .mIsin = "XX0000000001",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2008-01-10")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-03-10")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 3'000'000'000, // EUR 30
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
        {
            .mName = "Synthetic Share B",
            .mIsin = "XX0000000002",
            .mTransactions =
                {
                    {.mMetadata = {.mTaxDate = day("2024-01-02")},
                     .mTradeSide = TradeSide::Buy,
                     .mUnitPrice = 1'000'000'000, // EUR 10
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                    {.mMetadata = {.mTaxDate = day("2024-02-01")},
                     .mTradeSide = TradeSide::Sell,
                     .mUnitPrice = 1'200'000'000, // EUR 12
                     .mUnits = 200'000'000,       // 2 units
                     .mCurrency = Currency::EUR},
                },
        },
    };

    const auto result = buildYear(merged, 2024, {"XX0000000001"});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);
    EXPECT_EQ(result.mReport->mLists.at(0).mIsin, "XX0000000002");

    // A is left out as a whole: its 2 units past the holding period are not listed either.
    EXPECT_TRUE(result.mLongHeld.empty());
    ASSERT_EQ(result.mExcluded.size(), 1U);
    EXPECT_EQ(result.mExcluded.at(0),
              (KdvpExcludedIsin{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share A",
                  .mBlockReason = std::nullopt,
                  .mSales = {{StatementEventKind::Trade, 0, 1}},
              }));
}

TEST(CapitalGainsReportBuilderTest, ASplitLotFromAnEarlierYearIsListedInUnitsAfterTheSplit2024) {
    // 1 unit bought at EUR 10 in 2022, split 2 / 1 in 2023, 2 units sold at EUR 30 in 2024. The
    // purchase row lists the 2 units after the split at EUR 10 / 2 = EUR 5 (the split_question.csv
    // shape), with the original purchase date.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2022-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-10")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-01-15")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2023-01-15")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);
    EXPECT_EQ(list.mRows.at(0),
              (KdvpRow{.mEntry = KdvpPurchaseRow{.mPurchase = {StatementEventKind::Trade, 0, 0},
                                                 .mDate = day("2022-01-10"),
                                                 .mMethod = KdvpPurchaseMethod::B,
                                                 .mUnits = 200'000'000,      // 2 units
                                                 .mUnitPrice = 500'000'000}, // EUR 5
                       .mStock = 200'000'000}));                             // 2
    EXPECT_EQ(list.mRows.at(1),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 1},
                                             .mDate = day("2024-03-10"),
                                             .mUnits = 200'000'000,       // 2 units
                                             .mUnitPrice = 3'000'000'000, // EUR 30
                                             .mLossRule = KdvpLossRule::NotBlocked},
                       .mStock = 0}));

    // 2 x (30 - 5) = EUR 50.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{eurTicks(50)}));
}

TEST(CapitalGainsReportBuilderTest, ASplitLotIsListedInUnitsAfterTheSplit2024) {
    // 10 units bought at EUR 100 on 2024-03-01, split 2 / 1 on 2024-06-03, all 20 units sold at
    // EUR 60 on 2024-09-02: one purchase row of 20 units at EUR 50 (tax_rules.md example).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 1'000'000'000,      // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-09-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 6'000'000'000, // EUR 60
                 .mUnits = 2'000'000'000,     // 20 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-06-03")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);
    EXPECT_EQ(list.mRows.at(0),
              (KdvpRow{.mEntry = KdvpPurchaseRow{.mPurchase = {StatementEventKind::Trade, 0, 0},
                                                 .mDate = day("2024-03-01"),
                                                 .mMethod = KdvpPurchaseMethod::B,
                                                 .mUnits = 2'000'000'000,      // 20 units
                                                 .mUnitPrice = 5'000'000'000}, // EUR 50
                       .mStock = 2'000'000'000}));                             // 20
    EXPECT_EQ(list.mRows.at(1),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 1},
                                             .mDate = day("2024-09-02"),
                                             .mUnits = 2'000'000'000,     // 20 units
                                             .mUnitPrice = 6'000'000'000, // EUR 60
                                             .mLossRule = KdvpLossRule::NotBlocked},
                       .mStock = 0}));

    // 20 x (60 - 50) = EUR 200 = 1,200 - 1,000.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{eurTicks(200)}));
}

TEST(CapitalGainsReportBuilderTest, OnlyTheSoldUnitsOfASplitLotAreListed2024) {
    // 10 units bought at EUR 100 in 2023, split 2 / 1 in 2023 (20 units at EUR 50), 6 sold at
    // EUR 70 in 2024. The 14 units still held are not listed.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 1'000'000'000,      // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 7'000'000'000, // EUR 70
                 .mUnits = 600'000'000,       // 6 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2023-06-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2023-06-01")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);
    EXPECT_EQ(purchaseAt(list, 0).mUnits, 600'000'000);       // 6 of the 20 units, F3
    EXPECT_EQ(purchaseAt(list, 0).mUnitPrice, 5'000'000'000); // EUR 50, F4
    EXPECT_EQ(list.mRows.at(0).mStock, 600'000'000);          // F8 = 6
    EXPECT_EQ(saleAt(list, 1).mUnits, 600'000'000);           // F7
    EXPECT_EQ(list.mRows.at(1).mStock, 0);

    // 6 x (70 - 50) = EUR 120.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{eurTicks(120)}));
}

TEST(CapitalGainsReportBuilderTest, RoundedUnitValuesOfAFiveForThreeSplit2024) {
    // fifo.md "Exact distribution across several lots": 1 unit and 2 units, each unit bought at
    // EUR 10, split 5 / 3. The lots become 1.66666667 and 3.33333333 units with the unchanged
    // bases EUR 10 and EUR 20, so F4 is 10 / 1.66666667 = 5.999999988 -> 5.99999999 and
    // 20 / 3.33333333 = 6.000000006 -> 6.00000001. All 5 units are sold at EUR 7.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-12")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-05-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 700'000'000, // EUR 7
                 .mUnits = 500'000'000,     // 5 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-04-02")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 5, .mOldShares = 3},
                               .mEffectiveDate = day("2024-04-02")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 3U);
    EXPECT_EQ(purchaseAt(list, 0).mUnits, 166'666'667);     // 1.66666667 units
    EXPECT_EQ(purchaseAt(list, 0).mUnitPrice, 599'999'999); // EUR 5.99999999
    EXPECT_EQ(list.mRows.at(0).mStock, 166'666'667);
    EXPECT_EQ(purchaseAt(list, 1).mUnits, 333'333'333);     // 3.33333333 units
    EXPECT_EQ(purchaseAt(list, 1).mUnitPrice, 600'000'001); // EUR 6.00000001
    EXPECT_EQ(list.mRows.at(1).mStock, 500'000'000);        // F8 = 5
    EXPECT_EQ(saleAt(list, 2).mUnits, 500'000'000);         // 5 units
    EXPECT_EQ(list.mRows.at(2).mStock, 0);

    // 1.66666667 x (7 - 5.99999999) + 3.33333333 x (7 - 6.00000001)
    // = 1.6666666866666667 + 3.3333332966666667 = EUR 4.9999999833333334, in ticks of 10^-16 EUR.
    // The exact gain is 35 - 30 = EUR 5; the F4 rounding moves it by less than EUR 0.0000001.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{WideInteger{49'999'999'833'333'334}}));
}

TEST(CapitalGainsReportBuilderTest, RoundedUnitValueOfAOneForThreeReverseSplit2024) {
    // fifo.md "A unit-only action with a sub-step residual": 1 unit bought at EUR 10, reverse
    // split 1 / 3 gives 0.33333333 units with the unchanged basis EUR 10, so F4 is
    // 10 / 0.33333333 = 30.00000030. All 0.33333333 units are sold at EUR 31.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'100'000'000, // EUR 31
                 .mUnits = 33'333'333,        // 0.33333333 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 3},
                               .mEffectiveDate = day("2024-03-01")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);
    EXPECT_EQ(purchaseAt(list, 0).mUnits, 33'333'333);        // 0.33333333 units
    EXPECT_EQ(purchaseAt(list, 0).mUnitPrice, 3'000'000'030); // EUR 30.00000030
    EXPECT_EQ(list.mRows.at(0).mStock, 33'333'333);
    EXPECT_EQ(saleAt(list, 1).mUnits, 33'333'333);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);

    // 0.33333333 x 31 = 10.33333323 minus 0.33333333 x 30.0000003 = 9.999999999999999, so
    // EUR 0.333333230000001 = 3'333'332'300'000'010 ticks of 10^-16 EUR.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{WideInteger{3'333'332'300'000'010}}));
}

TEST(CapitalGainsReportBuilderTest, APurchaseSoldBeforeAndAfterASplitHasTwoRows2024) {
    // 10 units bought at EUR 100 on 2023-05-02. 4 are sold at EUR 120 on 2024-02-01 (old units),
    // the split 2 / 1 on 2024-06-03 makes the 6 left 12 units at EUR 50, and 12 are sold at
    // EUR 55 on 2024-09-02. The purchase has one row for each unit state, at its position.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-05-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 1'000'000'000,      // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 12'000'000'000, // EUR 120
                 .mUnits = 400'000'000,        // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-09-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 5'500'000'000, // EUR 55
                 .mUnits = 1'200'000'000,     // 12 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-06-03")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 4U);
    EXPECT_EQ(list.mRows.at(0),
              (KdvpRow{.mEntry = KdvpPurchaseRow{.mPurchase = {StatementEventKind::Trade, 0, 0},
                                                 .mDate = day("2023-05-02"),
                                                 .mMethod = KdvpPurchaseMethod::B,
                                                 .mUnits = 400'000'000,         // 4 old units
                                                 .mUnitPrice = 10'000'000'000}, // EUR 100
                       .mStock = 400'000'000}));                                // 4
    EXPECT_EQ(list.mRows.at(1),
              (KdvpRow{.mEntry = KdvpPurchaseRow{.mPurchase = {StatementEventKind::Trade, 0, 0},
                                                 .mDate = day("2023-05-02"),
                                                 .mMethod = KdvpPurchaseMethod::B,
                                                 .mUnits = 1'200'000'000,      // 12 new units
                                                 .mUnitPrice = 5'000'000'000}, // EUR 50
                       .mStock = 1'600'000'000}));                             // 16
    EXPECT_EQ(list.mRows.at(2),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 1},
                                             .mDate = day("2024-02-01"),
                                             .mUnits = 400'000'000,        // 4 units
                                             .mUnitPrice = 12'000'000'000, // EUR 120
                                             .mLossRule = KdvpLossRule::NotBlocked},
                       .mStock = 1'200'000'000})); // 16 - 4
    EXPECT_EQ(list.mRows.at(3),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 2},
                                             .mDate = day("2024-09-02"),
                                             .mUnits = 1'200'000'000,     // 12 units
                                             .mUnitPrice = 5'500'000'000, // EUR 55
                                             .mLossRule = KdvpLossRule::NotBlocked},
                       .mStock = 0}));

    // 4 x (120 - 100) = EUR 80 and 12 x (55 - 50) = EUR 60.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{eurTicks(80), eurTicks(60)}));
}

TEST(CapitalGainsReportBuilderTest,
     ASplitAndAReverseSplitThatCancelGiveThreeRowsOfOnePurchase2024) {
    // 10 units bought at EUR 100 on 2023-05-02. 4 are sold at EUR 120 on 2024-02-01 (old units).
    // The split 2 / 1 on 2024-06-03 makes the 6 left 12 units at EUR 50; 4 of them are sold at
    // EUR 55 on 2024-07-01 (8 left). The reverse split 1 / 2 on 2024-08-01 makes them 4 units at
    // EUR 100 again; 4 are sold at EUR 130 on 2024-09-02. The units sold before and after the
    // two actions have the same unit value but are not neighbours, so the purchase has three
    // rows, and a FIFO over the rows keeps the gain of every sale.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-05-02")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 10'000'000'000, // EUR 100
                 .mUnits = 1'000'000'000,      // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 12'000'000'000, // EUR 120
                 .mUnits = 400'000'000,        // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-07-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 5'500'000'000, // EUR 55
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-09-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 13'000'000'000, // EUR 130
                 .mUnits = 400'000'000,        // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03")},
                 .mType = CorporateActionType::UnresolvedSplit},
                {.mMetadata = {.mTaxDate = day("2024-08-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-06-03")},
                              {.mEvents = {{StatementEventKind::CorporateAction, 0, 1}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 2},
                               .mEffectiveDate = day("2024-08-01")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 6U);
    EXPECT_EQ(list.mRows.at(0),
              (KdvpRow{.mEntry = KdvpPurchaseRow{.mPurchase = {StatementEventKind::Trade, 0, 0},
                                                 .mDate = day("2023-05-02"),
                                                 .mMethod = KdvpPurchaseMethod::B,
                                                 .mUnits = 400'000'000,         // 4 old units
                                                 .mUnitPrice = 10'000'000'000}, // EUR 100
                       .mStock = 400'000'000}));                                // 4
    EXPECT_EQ(list.mRows.at(1),
              (KdvpRow{.mEntry = KdvpPurchaseRow{.mPurchase = {StatementEventKind::Trade, 0, 0},
                                                 .mDate = day("2023-05-02"),
                                                 .mMethod = KdvpPurchaseMethod::B,
                                                 .mUnits = 400'000'000,        // 4 new units
                                                 .mUnitPrice = 5'000'000'000}, // EUR 50
                       .mStock = 800'000'000}));                               // 4 + 4
    EXPECT_EQ(list.mRows.at(2),
              (KdvpRow{.mEntry = KdvpPurchaseRow{.mPurchase = {StatementEventKind::Trade, 0, 0},
                                                 .mDate = day("2023-05-02"),
                                                 .mMethod = KdvpPurchaseMethod::B,
                                                 .mUnits = 400'000'000,         // 4 old units
                                                 .mUnitPrice = 10'000'000'000}, // EUR 100
                       .mStock = 1'200'000'000}));                              // 8 + 4
    EXPECT_EQ(list.mRows.at(3),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 1},
                                             .mDate = day("2024-02-01"),
                                             .mUnits = 400'000'000,        // 4 units
                                             .mUnitPrice = 12'000'000'000, // EUR 120
                                             .mLossRule = KdvpLossRule::NotBlocked},
                       .mStock = 800'000'000})); // 12 - 4
    EXPECT_EQ(list.mRows.at(4),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 2},
                                             .mDate = day("2024-07-01"),
                                             .mUnits = 400'000'000,       // 4 units
                                             .mUnitPrice = 5'500'000'000, // EUR 55
                                             .mLossRule = KdvpLossRule::NotBlocked},
                       .mStock = 400'000'000})); // 8 - 4
    EXPECT_EQ(list.mRows.at(5),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 3},
                                             .mDate = day("2024-09-02"),
                                             .mUnits = 400'000'000,        // 4 units
                                             .mUnitPrice = 13'000'000'000, // EUR 130
                                             .mLossRule = KdvpLossRule::NotBlocked},
                       .mStock = 0}));

    // 4 x (120 - 100) = EUR 80, 4 x (55 - 50) = EUR 20 and 4 x (130 - 100) = EUR 120.
    EXPECT_EQ(gainsOfRows(list),
              (std::vector<WideInteger>{eurTicks(80), eurTicks(20), eurTicks(120)}));
}

TEST(CapitalGainsReportBuilderTest, AnUnchangedLotBeforeASplitAndAChangedLotAfterIt2024) {
    // 5 units at EUR 10 and 5 units at EUR 20. 5 are sold at EUR 12 on 2024-02-01 (the first
    // lot). The split 2 / 1 on 2024-06-03 makes the second lot 10 units at EUR 10; 10 are sold
    // at EUR 11 on 2024-09-02. F8 mixes the old units before the split with the new ones after.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2023-03-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-04-03")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'200'000'000, // EUR 12
                 .mUnits = 500'000'000,       // 5 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-09-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'100'000'000, // EUR 11
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-06-03")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-06-03")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 4U);
    EXPECT_EQ(purchaseAt(list, 0).mUnits, 500'000'000);       // 5 units, F3
    EXPECT_EQ(purchaseAt(list, 0).mUnitPrice, 1'000'000'000); // EUR 10, F4
    EXPECT_EQ(list.mRows.at(0).mStock, 500'000'000);          // F8 = 5
    EXPECT_EQ(purchaseAt(list, 1).mUnits, 1'000'000'000);     // 10 new units, F3
    EXPECT_EQ(purchaseAt(list, 1).mUnitPrice, 1'000'000'000); // EUR 20 / 2 = EUR 10, F4
    EXPECT_EQ(list.mRows.at(1).mStock, 1'500'000'000);        // F8 = 15
    EXPECT_EQ(saleAt(list, 2).mUnits, 500'000'000);           // 5 units, F7
    EXPECT_EQ(list.mRows.at(2).mStock, 1'000'000'000);        // F8 = 10
    EXPECT_EQ(saleAt(list, 3).mUnits, 1'000'000'000);         // 10 units, F7
    EXPECT_EQ(list.mRows.at(3).mStock, 0);

    // 5 x (12 - 10) = EUR 10 and 10 x (11 - 10) = EUR 10.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{eurTicks(10), eurTicks(10)}));
}

TEST(CapitalGainsReportBuilderTest, LongHeldUnitsOfASplitLotHaveNoRow2024) {
    // 2 units at EUR 10 (2008) and 2 units at EUR 20 (2020), split 2 / 1 in 2022, 6 of the 8
    // units sold at EUR 15 on 2024-03-11. The first 4 units are from 2008 (past 15 years, left
    // out); the other 2 are units of the 2020 lot, now at EUR 40 / 4 = EUR 10.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2020-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 2'000'000'000, // EUR 20
                 .mUnits = 200'000'000,       // 2 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-11")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'500'000'000, // EUR 15
                 .mUnits = 600'000'000,       // 6 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2022-03-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2022-03-01")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);
    EXPECT_EQ(purchaseAt(list, 0).mDate, day("2020-01-10"));
    EXPECT_EQ(purchaseAt(list, 0).mUnits, 200'000'000);       // 2 units, F3
    EXPECT_EQ(purchaseAt(list, 0).mUnitPrice, 1'000'000'000); // EUR 10, F4
    EXPECT_EQ(list.mRows.at(0).mStock, 200'000'000);          // F8 = 2
    EXPECT_EQ(saleAt(list, 1).mUnits, 200'000'000);           // 2 units, F7
    EXPECT_EQ(list.mRows.at(1).mStock, 0);

    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0).mUnits, 400'000'000); // the 4 units of 2008

    // 2 x (15 - 10) = EUR 10.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{eurTicks(10)}));
}

TEST(CapitalGainsReportBuilderTest, TheExampleAcrossSeveralYearsGivesItsRows2024) {
    // tax_rules.md "Example across several years" with prices. The 2008 units (12 after the
    // split) are past 15 years and left out. The sale of 4 units at EUR 15 on 2024-12-20 is a
    // loss against the EUR 20 unit value, and the purchase of 4 units on 2025-01-05 blocks it.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2008-05-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 500'000'000, // EUR 5
                 .mUnits = 1'000'000'000,   // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-03-01")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 600'000'000, // EUR 6
                 .mUnits = 400'000'000,     // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2023-09-01")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 4'000'000'000, // EUR 40
                 .mUnits = 800'000'000,       // 8 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-06-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 2'500'000'000, // EUR 25
                 .mUnits = 2'000'000'000,     // 20 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-12-20")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 1'500'000'000, // EUR 15
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2025-01-05")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'500'000'000, // EUR 15
                 .mUnits = 400'000'000,       // 4 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-02-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-02-01")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 3U);
    EXPECT_EQ(list.mRows.at(0),
              (KdvpRow{.mEntry = KdvpPurchaseRow{.mPurchase = {StatementEventKind::Trade, 0, 2},
                                                 .mDate = day("2023-09-01"),
                                                 .mMethod = KdvpPurchaseMethod::B,
                                                 .mUnits = 1'200'000'000,      // 12 units
                                                 .mUnitPrice = 2'000'000'000}, // EUR 40 / 2
                       .mStock = 1'200'000'000}));                             // 12
    EXPECT_EQ(list.mRows.at(1),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 3},
                                             .mDate = day("2024-06-20"),
                                             .mUnits = 800'000'000,       // 8 of the 20 units
                                             .mUnitPrice = 2'500'000'000, // EUR 25
                                             .mLossRule = KdvpLossRule::NotBlocked},
                       .mStock = 400'000'000})); // 12 - 8
    EXPECT_EQ(list.mRows.at(2),
              (KdvpRow{.mEntry = KdvpSaleRow{.mSale = {StatementEventKind::Trade, 0, 4},
                                             .mDate = day("2024-12-20"),
                                             .mUnits = 400'000'000,       // 4 units
                                             .mUnitPrice = 1'500'000'000, // EUR 15
                                             .mLossRule = KdvpLossRule::Blocked},
                       .mStock = 0}));

    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0).mUnits, 1'200'000'000); // the 12 units of 2008

    // 8 x (25 - 20) = +EUR 40 and 4 x (15 - 20) = -EUR 20.
    EXPECT_EQ(gainsOfRows(list), (std::vector<WideInteger>{eurTicks(40), eurTicks(-20)}));
}

TEST(CapitalGainsReportBuilderTest, AnAdjustedUnitValueThatDoesNotFitBlocksTheIsin2024) {
    // 1 unit bought at EUR 92,233,720,368 (just below the largest UnitPrice), reverse split 1 / 2
    // on 2024-03-01: 0.5 units with the same basis, so EUR 184,467,440,736 a unit, which a
    // UnitPrice cannot hold (F4 would have 12 integer digits, UnitPrice stops at EUR 92 billion).
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 9'223'372'036'800'000'000, // EUR 92,233,720,368
                 .mUnits = 100'000'000,                   // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 9'223'372'036'800'000'000, // EUR 92,233,720,368
                 .mUnits = 50'000'000,                    // 0.5 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 1, .mOldShares = 2},
                               .mEffectiveDate = day("2024-03-01")}});

    EXPECT_FALSE(result.mReport.has_value());
    ASSERT_EQ(result.mBlocked.size(), 1U);
    EXPECT_EQ(result.mBlocked.at(0),
              (KdvpBlockedIsin{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mReason = KdvpBlockReason::UnitValueTooLarge,
                  .mSales = {{StatementEventKind::Trade, 0, 1}},
              }));
}

TEST(CapitalGainsReportBuilderTest, AnAdjustedUnitValueBelowTheLastDecimalIsWrittenAsZero2024) {
    // 1 unit bought at EUR 0.0001, split 1,000,000 / 1 on 2024-03-01: 1,000,000 units with the
    // same basis, so EUR 0.0000000001 a unit. Rounded once to 8 decimals, F4 is 0.00000000; the
    // row is written as it is (calculations.md). All units are sold at EUR 0.0001.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2024-01-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 10'000,  // EUR 0.0001
                 .mUnits = 100'000'000, // 1 unit
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-04-02")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 10'000,          // EUR 0.0001
                 .mUnits = 100'000'000'000'000, // 1,000,000 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-03-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 1'000'000, .mOldShares = 1},
                               .mEffectiveDate = day("2024-03-01")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);
    EXPECT_EQ(purchaseAt(list, 0).mUnits, 100'000'000'000'000); // 1,000,000 units
    EXPECT_EQ(purchaseAt(list, 0).mUnitPrice, 0);               // EUR 0.0000000001 -> 0.00000000
    EXPECT_EQ(list.mRows.at(0).mStock, 100'000'000'000'000);    // F8 = 1,000,000
    EXPECT_EQ(saleAt(list, 1).mUnits, 100'000'000'000'000);
    EXPECT_EQ(list.mRows.at(1).mStock, 0);
}

TEST(CapitalGainsReportBuilderTest,
     ARuleThatBlocksMoreUnitsThanTheSaleHasLossUnitsBlocksTheIsin2024) {
    // The sale of 15 units at EUR 18 has 5 loss units (the EUR 20 purchase). The loss result is
    // changed by hand to block 6 units, which no sale can have: the sale is not written as rows.
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
                 .mUnits = 500'000'000,       // 5 units
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
            },
    }};

    orderChronologically(merged.mStatement);

    const auto prepared = TaxProcessor{}.prepareReportingYear({2024, merged});
    const auto& ledger = prepared.mLedger.value();
    const auto fifo = FifoMatcher{}.match(merged.mStatement, ledger);
    RunDecisions decisions;
    auto loss = LossEligibilityChecker::check(merged.mStatement, ledger, fifo, decisions);

    ASSERT_EQ(loss.mIsins.at(0).mSales.size(), 1U);
    EXPECT_EQ(loss.mIsins.at(0).mSales.at(0).ineligibleUnits(), 500'000'000); // 5 units

    loss.mIsins.at(0).mSales.at(0).mKnownIneligibleUnits = 600'000'000; // 6 units

    const auto result =
        CapitalGainsReportBuilder{}.build(merged.mStatement, ledger, fifo, loss, {});

    EXPECT_FALSE(result.mReport.has_value());
    ASSERT_EQ(result.mBlocked.size(), 1U);
    EXPECT_EQ(result.mBlocked.at(0),
              (KdvpBlockedIsin{
                  .mIsin = "XX0000000001",
                  .mName = "Synthetic Share",
                  .mReason = KdvpBlockReason::LossNotReportable,
                  .mSales = {{StatementEventKind::Trade, 0, 2}},
              }));
}

TEST(CapitalGainsReportBuilderTest, ASaleBeforeASplitLaterInTheYearIsListed) {
    // 2 units bought, 1 sold on 2024-03-10, split 2 / 1 on 2024-09-01. The sale uses a lot the
    // split had not changed yet, so its rows are the usual ones: 1 unit at EUR 10, then 1 sold.
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
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 100'000'000,       // 1 unit
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2024-09-01")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-09-01")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    ASSERT_EQ(result.mReport->mLists.size(), 1U);

    const auto& list = result.mReport->mLists.at(0);

    ASSERT_EQ(list.mRows.size(), 2U);
    EXPECT_EQ(purchaseAt(list, 0).mUnits, 100'000'000);       // 1 of the 2 units, F3
    EXPECT_EQ(purchaseAt(list, 0).mUnitPrice, 1'000'000'000); // EUR 10, F4
    EXPECT_EQ(list.mRows.at(0).mStock, 100'000'000);          // F8 = 1
    EXPECT_EQ(saleAt(list, 1).mUnits, 100'000'000);           // F7
    EXPECT_EQ(saleAt(list, 1).mUnitPrice, 3'000'000'000);     // EUR 30, F9
    EXPECT_EQ(list.mRows.at(1).mStock, 0);                    // F8 = 1 - 1
}

TEST(CapitalGainsReportBuilderTest, ChangedUnitsPastFifteenYearsDoNotBlock2024) {
    // 10 units bought 2009-03-10, split 2 / 1 in 2015, all 20 sold on 2024-03-11, past the 15
    // years: they get no row, so the changed lot does not block. The 20 units stay listed as
    // tax free.
    StatementMergeResult merged;
    merged.mStatement.mPresentation.mTradeInstruments = {{
        .mName = "Synthetic Share",
        .mIsin = "XX0000000001",
        .mTransactions =
            {
                {.mMetadata = {.mTaxDate = day("2009-03-10")},
                 .mTradeSide = TradeSide::Buy,
                 .mUnitPrice = 1'000'000'000, // EUR 10
                 .mUnits = 1'000'000'000,     // 10 units
                 .mCurrency = Currency::EUR},
                {.mMetadata = {.mTaxDate = day("2024-03-11")},
                 .mTradeSide = TradeSide::Sell,
                 .mUnitPrice = 3'000'000'000, // EUR 30
                 .mUnits = 2'000'000'000,     // 20 units
                 .mCurrency = Currency::EUR},
            },
        .mCorporateActions =
            {
                {.mMetadata = {.mTaxDate = day("2015-01-15")},
                 .mType = CorporateActionType::UnresolvedSplit},
            },
    }};

    const auto result =
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2015-01-15")}});

    EXPECT_TRUE(result.mBlocked.empty());
    ASSERT_TRUE(result.mReport.has_value());
    EXPECT_TRUE(result.mReport->mLists.empty());
    ASSERT_EQ(result.mLongHeld.size(), 1U);
    EXPECT_EQ(result.mLongHeld.at(0).mUnits, 2'000'000'000); // 20 units
}

TEST(CapitalGainsReportBuilderTest, ALossSaleBeforeAnAppliedSplitInItsWindowBlocksForTheLossRule) {
    // 4 units bought, 3 sold at a loss on 2024-11-20, split 2 / 1 on 2024-12-01 inside the
    // window. The sale uses an unchanged lot, so the rows are not the problem; the loss rule is.
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
        buildYearWithActions(merged,
                             2024,
                             {{.mEvents = {{StatementEventKind::CorporateAction, 0, 0}},
                               .mRatio = {.mNewShares = 2, .mOldShares = 1},
                               .mEffectiveDate = day("2024-12-01")}});

    ASSERT_EQ(result.mBlocked.size(), 1U);
    EXPECT_EQ(result.mBlocked.at(0).mReason, KdvpBlockReason::LossNotReportable);
    EXPECT_EQ(result.mBlocked.at(0).mSales,
              (std::vector<StatementEventReference>{{StatementEventKind::Trade, 0, 1}}));
}

} // namespace
