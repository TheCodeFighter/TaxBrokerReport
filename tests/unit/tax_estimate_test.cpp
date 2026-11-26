#include "processors/tax_estimate.hpp"
#include "taxbroker/text_format.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

/// The estimate of a year with only a Doh-KDVP report.
TaxEstimate estimateCapitalOnly(int aYear, const CapitalGainsReportKDVP& aCapital) {
    return estimateTax(aYear, aCapital, {}, std::nullopt, {});
}

/// The estimate of a year with only a Doh-Div report.
TaxEstimate estimateDividendsOnly(int aYear, const DividendReportDiv& aDividends) {
    return estimateTax(aYear, {}, aDividends, std::nullopt, {});
}

/// The estimate of a year with only a Doh-Obr report.
TaxEstimate estimateOtherOnly(int aYear, const OtherInterestReportObr& aOther) {
    return estimateTax(aYear, {}, {}, std::nullopt, aOther);
}

// ---------------------------------------------------------------------------------------------
// Doh-KDVP
// ---------------------------------------------------------------------------------------------

TEST(TaxEstimateCapital, UnderFiveYearsIsTaxedAt25Percent) {
    // Buy 10 at 100.00 on 2020-03-02, sell 10 at 150.00 on 2024-06-03 (4 years 3 months).
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-03-02"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-06-03"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_TRUE(estimate.mYearSupported);
    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mForm, ReportType::CapitalGains);
    // Sale 1,500.00, purchase 1,000.00, costs 10.00 + 15.00, gain 475.00, 25 % = 118.75.
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{1'187'500});
    EXPECT_FALSE(estimate.mForms[0].mNotAssessed);
    EXPECT_FALSE(estimate.mForms[0].mIncomplete);
}

TEST(TaxEstimateCapital, FiveToTenYearsIsTaxedAt20Percent) {
    // Buy 10 at 100.00 on 2017-01-10, sell 10 at 150.00 on 2024-02-01 (7 years).
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2017-01-10"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-02-01"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    // Gain 475.00 at 20 % = 95.00.
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{950'000});
}

TEST(TaxEstimateCapital, LossOffsetsTheGainOfTheSameBracket) {
    // ISIN 1: gain 475.00 (under 5 years). ISIN 2: buy 10 at 100.00 on 2023-01-10, sell 10 at
    // 80.00 on 2024-04-02: 800.00 - 1,000.00 - 10.00 - 8.00 = loss 218.00.
    const CapitalGainsReportKDVP report{
        .mLists = {
            {.mIsin = "US0000000001",
             .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-03-02"),
                                                  .mUnits = 10 * UNITS_SCALE,
                                                  .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                       {.mEntry = KdvpSaleRow{.mDate = day("2024-06-03"),
                                              .mUnits = 10 * UNITS_SCALE,
                                              .mUnitPrice = 150 * UNIT_PRICE_SCALE}}}},
            {.mIsin = "US0000000002",
             .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2023-01-10"),
                                                  .mUnits = 10 * UNITS_SCALE,
                                                  .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                       {.mEntry = KdvpSaleRow{.mDate = day("2024-04-02"),
                                              .mUnits = 10 * UNITS_SCALE,
                                              .mUnitPrice = 80 * UNIT_PRICE_SCALE,
                                              .mLossRule = KdvpLossRule::NotBlocked}}}},
        }};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    // Base 475.00 - 218.00 = 257.00, 25 % = 64.25.
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{642'500});
}

TEST(TaxEstimateCapital, BlockedLossIsIgnored) {
    // The same two ISINs, but the 30-day rule blocks the loss (F10 false).
    const CapitalGainsReportKDVP report{
        .mLists = {
            {.mIsin = "US0000000001",
             .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-03-02"),
                                                  .mUnits = 10 * UNITS_SCALE,
                                                  .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                       {.mEntry = KdvpSaleRow{.mDate = day("2024-06-03"),
                                              .mUnits = 10 * UNITS_SCALE,
                                              .mUnitPrice = 150 * UNIT_PRICE_SCALE}}}},
            {.mIsin = "US0000000002",
             .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2023-01-10"),
                                                  .mUnits = 10 * UNITS_SCALE,
                                                  .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                       {.mEntry = KdvpSaleRow{.mDate = day("2024-04-02"),
                                              .mUnits = 10 * UNITS_SCALE,
                                              .mUnitPrice = 80 * UNIT_PRICE_SCALE,
                                              .mLossRule = KdvpLossRule::Blocked}}}},
        }};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    // The loss does not count: gain 475.00 at 25 % = 118.75.
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{1'187'500});
}

TEST(TaxEstimateCapital, LossIsSplitInProportionToTheGainsOfEachBracket) {
    // ISIN 1: gain 475.00 at 25 %. ISIN 2: buy 10 at 100.00 on 2017-01-10, sell 10 at 150.00 on
    // 2024-02-01: gain 475.00 at 20 %. ISIN 3: loss 218.00 (as in the test above).
    const CapitalGainsReportKDVP report{
        .mLists = {
            {.mIsin = "US0000000001",
             .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-03-02"),
                                                  .mUnits = 10 * UNITS_SCALE,
                                                  .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                       {.mEntry = KdvpSaleRow{.mDate = day("2024-06-03"),
                                              .mUnits = 10 * UNITS_SCALE,
                                              .mUnitPrice = 150 * UNIT_PRICE_SCALE}}}},
            {.mIsin = "US0000000002",
             .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2017-01-10"),
                                                  .mUnits = 10 * UNITS_SCALE,
                                                  .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                       {.mEntry = KdvpSaleRow{.mDate = day("2024-02-01"),
                                              .mUnits = 10 * UNITS_SCALE,
                                              .mUnitPrice = 150 * UNIT_PRICE_SCALE}}}},
            {.mIsin = "US0000000003",
             .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2023-01-10"),
                                                  .mUnits = 10 * UNITS_SCALE,
                                                  .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                       {.mEntry = KdvpSaleRow{.mDate = day("2024-04-02"),
                                              .mUnits = 10 * UNITS_SCALE,
                                              .mUnitPrice = 80 * UNIT_PRICE_SCALE}}}},
        }};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    // Each gain is half of 950.00, so each gets 109.00 of the loss: 366.00 x 25 % + 366.00 x 20 %
    // = 91.50 + 73.20 = 164.70.
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{1'647'000});
}

TEST(TaxEstimateCapital, UnevenGainsShareTheLossInTheirProportion) {
    // ISIN 1: gain 475.00 at 25 %.
    // ISIN 2: buy 5 at 100.00 on 2014-03-03, sell 5 at 150.00 on 2024-03-04 (10 to 15 years):
    //   742.50 - 505.00 = gain 237.50 at 15 %.
    // ISIN 3: buy 1 at 195.00 on 2023-01-10, sell 1 at 55.00 on 2024-04-02:
    //   54.45 - 196.95 = loss 142.50.
    const CapitalGainsReportKDVP report{
        .mLists = {
            {.mIsin = "US0000000001",
             .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-03-02"),
                                                  .mUnits = 10 * UNITS_SCALE,
                                                  .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                       {.mEntry = KdvpSaleRow{.mDate = day("2024-06-03"),
                                              .mUnits = 10 * UNITS_SCALE,
                                              .mUnitPrice = 150 * UNIT_PRICE_SCALE}}}},
            {.mIsin = "US0000000002",
             .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2014-03-03"),
                                                  .mUnits = 5 * UNITS_SCALE,
                                                  .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                       {.mEntry = KdvpSaleRow{.mDate = day("2024-03-04"),
                                              .mUnits = 5 * UNITS_SCALE,
                                              .mUnitPrice = 150 * UNIT_PRICE_SCALE}}}},
            {.mIsin = "US0000000003",
             .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2023-01-10"),
                                                  .mUnits = 1 * UNITS_SCALE,
                                                  .mUnitPrice = 195 * UNIT_PRICE_SCALE}},
                       {.mEntry = KdvpSaleRow{.mDate = day("2024-04-02"),
                                              .mUnits = 1 * UNITS_SCALE,
                                              .mUnitPrice = 55 * UNIT_PRICE_SCALE}}}},
        }};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    // The gains are 712.50, so the shares are 2/3 and 1/3 and the loss parts are 95.00 and 47.50.
    // Bases 380.00 and 190.00: 95.00 + 28.50 = 123.50.
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{1'235'000});
}

TEST(TaxEstimateCapital, LossLargerThanTheGainsLeavesNoTax) {
    // ISIN 1: gain 475.00. ISIN 2: buy 10 at 100.00 on 2023-01-10, sell 10 at 40.00 on 2024-04-02:
    // 396.00 - 1,010.00 = loss 614.00.
    const CapitalGainsReportKDVP report{
        .mLists = {
            {.mIsin = "US0000000001",
             .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-03-02"),
                                                  .mUnits = 10 * UNITS_SCALE,
                                                  .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                       {.mEntry = KdvpSaleRow{.mDate = day("2024-06-03"),
                                              .mUnits = 10 * UNITS_SCALE,
                                              .mUnitPrice = 150 * UNIT_PRICE_SCALE}}}},
            {.mIsin = "US0000000002",
             .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2023-01-10"),
                                                  .mUnits = 10 * UNITS_SCALE,
                                                  .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                       {.mEntry = KdvpSaleRow{.mDate = day("2024-04-02"),
                                              .mUnits = 10 * UNITS_SCALE,
                                              .mUnitPrice = 40 * UNIT_PRICE_SCALE}}}},
        }};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{0});
    // A tax of 0 is not "assessed up to EUR 10": there is nothing to say about it.
    EXPECT_FALSE(estimate.mForms[0].mNotAssessed);
}

TEST(TaxEstimateCapital, OnlyLossesLeaveNoTax) {
    // Buy 10 at 100.00 on 2023-01-10, sell 10 at 80.00 on 2024-04-02: loss 218.00, no gain.
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2023-01-10"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-04-02"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 80 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{0});
}

TEST(TaxEstimateCapital, TaxOfExactlyTenEuroIsNotAssessed) {
    // Buy 10 at 790.00 on 2023-05-02, sell 10 at 810.00 on 2024-05-02:
    // 8,100.00 - 7,900.00 - 79.00 - 81.00 = gain 40.00, 25 % = 10.00.
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2023-05-02"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 790 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-05-02"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 810 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{100'000}); // EUR 10.00
    EXPECT_TRUE(estimate.mForms[0].mNotAssessed);
}

TEST(TaxEstimateCapital, TaxAboveTenEuroIsAssessed) {
    // Buy 10 at 790.00 on 2023-05-02, sell 10 at 810.01 on 2024-05-02:
    // 8,100.10 x 0.99 = 8,019.099; 7,900.00 x 1.01 = 7,979.00; gain 40.099, 25 % = 10.02475,
    // which rounds to 10.02.
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2023-05-02"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 790 * UNIT_PRICE_SCALE}},
                      {.mEntry =
                           KdvpSaleRow{.mDate = day("2024-05-02"),
                                       .mUnits = 10 * UNITS_SCALE,
                                       .mUnitPrice = 81'001'000'000}}}, // 810.01 at 8 decimals
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{100'200}); // EUR 10.02
    EXPECT_FALSE(estimate.mForms[0].mNotAssessed);
}

TEST(TaxEstimateCapital, SaleTakesUnitsFromTheOldestPurchaseRowsFirst) {
    // Buy 5 at 100.00 on 2014-03-03 and 5 at 120.00 on 2022-03-01, sell 10 at 150.00 on
    // 2024-03-04. Part 1 (10 to 15 years, 15 %): 750.00 - 500.00 - 5.00 - 7.50 = 237.50, tax
    // 35.625. Part 2 (under 5 years, 25 %): 750.00 - 600.00 - 6.00 - 7.50 = 136.50, tax 34.125.
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2014-03-03"),
                                                 .mUnits = 5 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpPurchaseRow{.mDate = day("2022-03-01"),
                                                 .mUnits = 5 * UNITS_SCALE,
                                                 .mUnitPrice = 120 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-03-04"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    // 35.625 + 34.125 = 69.75.
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{697'500});
}

TEST(TaxEstimateCapital, TwoSalesUseTheOpenUnitsInOrder) {
    // Buy 10 at 100.00 on 2020-03-02. Sell 4 at 150.00 on 2024-03-04 and 6 at 90.00 on
    // 2024-05-06 (the second sale uses the 6 units that are left).
    // Sale 1: 594.00 - 404.00 = gain 190.00. Sale 2: 534.60 - 606.00 = loss 71.40.
    // Base 190.00 - 71.40 = 118.60, 25 % = 29.65.
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-03-02"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-03-04"),
                                             .mUnits = 4 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-05-06"),
                                             .mUnits = 6 * UNITS_SCALE,
                                             .mUnitPrice = 90 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{296'500}); // EUR 29.65
}

TEST(TaxEstimateCapital, SaleOnTheFifthAnniversaryIsStillUnderFiveYears) {
    // Buy 10 at 100.00 on 2019-06-03, sell 10 at 150.00 on 2024-06-03: gain 475.00.
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2019-06-03"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-06-03"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{1'187'500}); // 25 % = EUR 118.75
}

TEST(TaxEstimateCapital, SaleTheDayAfterTheFifthAnniversaryIsTaxedAt20Percent) {
    // The same, sold one day later on 2024-06-04.
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2019-06-03"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-06-04"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{950'000}); // 20 % = EUR 95.00
}

TEST(TaxEstimateCapital, PurchaseOnTheLeapDayHasItsAnniversaryOnThe28th) {
    // Buy 10 at 100.00 on 2020-02-29. The fifth anniversary is 2025-02-28 (2025 has no 29th).
    // A sale on 2025-02-28 is still under 5 years: 25 % of 475.00 = 118.75.
    const CapitalGainsReportKDVP onTheDay{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-02-29"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2025-02-28"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};
    // A sale on 2025-03-01 is 5 to 10 years: 20 % of 475.00 = 95.00.
    const CapitalGainsReportKDVP dayAfter{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-02-29"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2025-03-01"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimateOnTheDay = estimateCapitalOnly(2025, onTheDay);
    const TaxEstimate estimateDayAfter = estimateCapitalOnly(2025, dayAfter);

    ASSERT_EQ(estimateOnTheDay.mForms.size(), 1U);
    EXPECT_EQ(estimateOnTheDay.mForms[0].mTax, std::optional<Money>{1'187'500});
    ASSERT_EQ(estimateDayAfter.mForms.size(), 1U);
    EXPECT_EQ(estimateDayAfter.mForms[0].mTax, std::optional<Money>{950'000});
}

TEST(TaxEstimateCapital, Year2020UsesTheRatesOf27Point5Percent) {
    // Buy 10 at 100.00 on 2018-03-02, sell 10 at 150.00 on 2020-06-03: gain 475.00 x 27.5 % =
    // 130.625, which rounds to 130.63.
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2018-03-02"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2020-06-03"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2020, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{1'306'300});
}

TEST(TaxEstimateCapital, Year2019TaxesTheFiveToTenYearsAt15Percent) {
    // Buy 10 at 100.00 on 2012-01-04, sell 10 at 150.00 on 2019-03-01 (7 years): 475.00 x 15 %.
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2012-01-04"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2019-03-01"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2019, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{712'500}); // EUR 71.25
}

TEST(TaxEstimateCapital, Year2019TaxesFifteenToTwentyYearsAt5Percent) {
    // Buy 10 at 100.00 on 2001-03-05, sell 10 at 150.00 on 2019-03-05 (18 years): 475.00 x 5 %.
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2001-03-05"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2019-03-05"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2019, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{237'500}); // EUR 23.75
}

TEST(TaxEstimateCapital, Year2019TaxesOverTwentyYearsAt0Percent) {
    // Buy 10 at 100.00 on 1995-03-05, sell 10 at 150.00 on 2019-03-05 (24 years).
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("1995-03-05"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2019-03-05"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2019, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{0});
}

TEST(TaxEstimateCapital, SaleOfUnitsThatWereNeverBoughtIsIncomplete) {
    // Sell 10 with only 4 units bought: the rows do not add up (a developer export).
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-03-02"),
                                                 .mUnits = 4 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-06-03"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_FALSE(estimate.mForms[0].mTax.has_value());
    EXPECT_TRUE(estimate.mForms[0].mIncomplete);
}

TEST(TaxEstimateCapital, UnsafeListIsIncomplete) {
    // A developer export of an unsafe list is never summed.
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mUnsafe = true,
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-03-02"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-06-03"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_FALSE(estimate.mForms[0].mTax.has_value());
    EXPECT_TRUE(estimate.mForms[0].mIncomplete);
}

TEST(TaxEstimateCapital, SumThatDoesNotFitIsIncomplete) {
    // The largest units at the largest price: the exact sums grow beyond 256 bits.
    const CapitalGainsReportKDVP report{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-03-02"),
                                                 .mUnits = std::numeric_limits<Units>::max(),
                                                 .mUnitPrice = 1}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-06-03"),
                                             .mUnits = std::numeric_limits<Units>::max(),
                                             .mUnitPrice = std::numeric_limits<UnitPrice>::max()}}},
        }}};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_FALSE(estimate.mForms[0].mTax.has_value());
    EXPECT_TRUE(estimate.mForms[0].mIncomplete);
}

TEST(TaxEstimateCapital, ListsWithoutRowsGiveNoForm) {
    const CapitalGainsReportKDVP report{};

    const TaxEstimate estimate = estimateCapitalOnly(2024, report);

    EXPECT_TRUE(estimate.mYearSupported);
    EXPECT_TRUE(estimate.mForms.empty());
}

// ---------------------------------------------------------------------------------------------
// Years without confirmed rates
// ---------------------------------------------------------------------------------------------

TEST(TaxEstimateYears, YearBeforeTheTableHasNoEstimate) {
    const DividendReportDiv dividends{.mRows = {{
                                          .mIsin = "US0000000001",
                                          .mDate = day("2015-05-04"),
                                          .mKind = DividendKind::Ordinary,
                                          .mGrossIncome = 100 * MONEY_SCALE,
                                      }}};

    const TaxEstimate estimate = estimateDividendsOnly(2015, dividends);

    EXPECT_FALSE(estimate.mYearSupported);
    EXPECT_TRUE(estimate.mForms.empty());
}

TEST(TaxEstimateYears, YearAfterTheTableHasNoEstimate) {
    const DividendReportDiv dividends{.mRows = {{
                                          .mIsin = "US0000000001",
                                          .mDate = day("2026-05-04"),
                                          .mKind = DividendKind::Ordinary,
                                          .mGrossIncome = 100 * MONEY_SCALE,
                                      }}};

    const TaxEstimate estimate = estimateDividendsOnly(2026, dividends);

    EXPECT_FALSE(estimate.mYearSupported);
    EXPECT_TRUE(estimate.mForms.empty());
}

// ---------------------------------------------------------------------------------------------
// Doh-Div
// ---------------------------------------------------------------------------------------------

TEST(TaxEstimateDividends, ForeignTaxCountsUpToFifteenPercentOfTheGross) {
    const DividendReportDiv dividends{
        .mRows = {
            // Gross 200.00, foreign tax 30.00 (exactly 15 %): 50.00 - 30.00 = 20.00.
            {.mIsin = "US0000000001",
             .mDate = day("2024-03-04"),
             .mKind = DividendKind::Ordinary,
             .mGrossIncome = 200 * MONEY_SCALE,
             .mWithholding = {.mForeignTax = 30 * MONEY_SCALE}},
            // Gross 100.00, foreign tax 35.00: the credit is capped at 15.00, so 25.00 - 15.00
            // = 10.00.
            {.mIsin = "US0000000002",
             .mDate = day("2024-03-05"),
             .mKind = DividendKind::Ordinary,
             .mGrossIncome = 100 * MONEY_SCALE,
             .mWithholding = {.mForeignTax = 35 * MONEY_SCALE}},
            // Gross 40.00, no foreign tax: 10.00.
            {.mIsin = "US0000000003",
             .mDate = day("2024-03-06"),
             .mKind = DividendKind::Ordinary,
             .mGrossIncome = 40 * MONEY_SCALE,
             .mWithholding = {.mForeignTax = 0, .mBlankInExport = true}},
        }};

    const TaxEstimate estimate = estimateDividendsOnly(2024, dividends);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mForm, ReportType::Dividends);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{400'000}); // 20.00 + 10.00 + 10.00
    EXPECT_FALSE(estimate.mForms[0].mNotAssessed);
}

TEST(TaxEstimateDividends, TaxOfTenEuroOrLessIsNotAssessed) {
    // Gross 30.00, foreign tax 4.50 (15 %): 7.50 - 4.50 = 3.00.
    const DividendReportDiv dividends{.mRows = {{
                                          .mIsin = "US0000000001",
                                          .mDate = day("2024-03-04"),
                                          .mKind = DividendKind::Ordinary,
                                          .mGrossIncome = 30 * MONEY_SCALE,
                                          .mWithholding = {.mForeignTax = 45'000},
                                      }}};

    const TaxEstimate estimate = estimateDividendsOnly(2024, dividends);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{30'000}); // EUR 3.00
    EXPECT_TRUE(estimate.mForms[0].mNotAssessed);
}

TEST(TaxEstimateDividends, Year2020UsesTheRateOf27Point5Percent) {
    // Gross 100.00, no foreign tax: 27.50.
    const DividendReportDiv dividends{
        .mRows = {{
            .mIsin = "US0000000001",
            .mDate = day("2020-03-04"),
            .mKind = DividendKind::Ordinary,
            .mGrossIncome = 100 * MONEY_SCALE,
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
        }}};

    const TaxEstimate estimate = estimateDividendsOnly(2020, dividends);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{275'000});
}

TEST(TaxEstimateDividends, SumThatDoesNotFitIsIncomplete) {
    // Five rows of the largest gross: the tax is about 1.15e15 EUR, above what Money can keep.
    const DividendReportDiv dividends{.mRows = {
                                          {.mIsin = "US0000000001",
                                           .mDate = day("2024-03-04"),
                                           .mKind = DividendKind::Ordinary,
                                           .mGrossIncome = std::numeric_limits<Money>::max()},
                                          {.mIsin = "US0000000002",
                                           .mDate = day("2024-03-04"),
                                           .mKind = DividendKind::Ordinary,
                                           .mGrossIncome = std::numeric_limits<Money>::max()},
                                          {.mIsin = "US0000000003",
                                           .mDate = day("2024-03-04"),
                                           .mKind = DividendKind::Ordinary,
                                           .mGrossIncome = std::numeric_limits<Money>::max()},
                                          {.mIsin = "US0000000004",
                                           .mDate = day("2024-03-04"),
                                           .mKind = DividendKind::Ordinary,
                                           .mGrossIncome = std::numeric_limits<Money>::max()},
                                          {.mIsin = "US0000000005",
                                           .mDate = day("2024-03-04"),
                                           .mKind = DividendKind::Ordinary,
                                           .mGrossIncome = std::numeric_limits<Money>::max()},
                                      }};

    const TaxEstimate estimate = estimateDividendsOnly(2024, dividends);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_FALSE(estimate.mForms[0].mTax.has_value());
    EXPECT_TRUE(estimate.mForms[0].mIncomplete);
}

// ---------------------------------------------------------------------------------------------
// Doh-DHO
// ---------------------------------------------------------------------------------------------

TEST(TaxEstimateDeposit, OnlyTheInterestAboveTheAllowanceIsTaxed) {
    // Qualifying interest 2,000.00 and the allowance 1,000.00: 1,000.00 x 25 % = 250.00.
    const DepositInterestSummary deposit{.mImportedQualifying = 2'000 * MONEY_SCALE,
                                         .mAllowance = 1'000 * MONEY_SCALE,
                                         .mAddOtherBanks = false};

    const TaxEstimate estimate = estimateTax(2024, {}, {}, deposit, {});

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mForm, ReportType::BankDepositInterest);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{2'500'000}); // EUR 250.00
}

TEST(TaxEstimateDeposit, InterestOf1500Gives125) {
    const DepositInterestSummary deposit{.mImportedQualifying = 1'500 * MONEY_SCALE,
                                         .mAllowance = 1'000 * MONEY_SCALE,
                                         .mAddOtherBanks = true};

    const TaxEstimate estimate = estimateTax(2024, {}, {}, deposit, {});

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{1'250'000}); // 500.00 x 25 %
}

TEST(TaxEstimateDeposit, InterestBelowTheAllowanceGivesNoTax) {
    const DepositInterestSummary deposit{.mImportedQualifying = 900 * MONEY_SCALE,
                                         .mAllowance = 1'000 * MONEY_SCALE,
                                         .mAddOtherBanks = false};

    const TaxEstimate estimate = estimateTax(2024, {}, {}, deposit, {});

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{0});
    EXPECT_FALSE(estimate.mForms[0].mNotAssessed);
}

// ---------------------------------------------------------------------------------------------
// Doh-Obr
// ---------------------------------------------------------------------------------------------

TEST(TaxEstimateOtherInterest, ForeignTaxCountsUpToFifteenPercentOfTheGross) {
    const OtherInterestReportObr other{
        .mRows = {
            // Gross 80.00, no foreign tax: 20.00.
            {.mKind = BrokerCashInterest{.mBroker = Broker::InteractiveBrokers},
             .mDate = day("2024-03-04"),
             .mGrossInterest = 80 * MONEY_SCALE,
             .mWithholding = {.mForeignTax = 0, .mBlankInExport = true}},
            // Gross 60.00, foreign tax 12.00 (20 %): the credit is capped at 9.00, so 15.00 - 9.00
            // =
            // 6.00.
            {.mKind = BondCoupon{.mIsin = "US0000000001"},
             .mDate = day("2024-03-05"),
             .mGrossInterest = 60 * MONEY_SCALE,
             .mWithholding = {.mForeignTax = 12 * MONEY_SCALE}},
        }};

    const TaxEstimate estimate = estimateOtherOnly(2024, other);

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mForm, ReportType::OtherInterest);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{260'000}); // 20.00 + 6.00
}

// ---------------------------------------------------------------------------------------------
// The forms together
// ---------------------------------------------------------------------------------------------

TEST(TaxEstimateTotal, FormsInReportOrderAddUpToTheTotal) {
    // Doh-KDVP 118.75 (the first test), Doh-Div 40.00, Doh-DHO 125.00 (interest 1,500.00) and
    // Doh-Obr 26.00.
    const CapitalGainsReportKDVP capital{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2020-03-02"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 100 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-06-03"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 150 * UNIT_PRICE_SCALE}}},
        }}};
    const DividendReportDiv dividends{
        .mRows = {
            {.mIsin = "US0000000002",
             .mDate = day("2024-03-04"),
             .mKind = DividendKind::Ordinary,
             .mGrossIncome = 200 * MONEY_SCALE,
             .mWithholding = {.mForeignTax = 30 * MONEY_SCALE}},
            {.mIsin = "US0000000003",
             .mDate = day("2024-03-05"),
             .mKind = DividendKind::Ordinary,
             .mGrossIncome = 100 * MONEY_SCALE,
             .mWithholding = {.mForeignTax = 35 * MONEY_SCALE}},
            {.mIsin = "US0000000004",
             .mDate = day("2024-03-06"),
             .mKind = DividendKind::Ordinary,
             .mGrossIncome = 40 * MONEY_SCALE,
             .mWithholding = {.mForeignTax = 0, .mBlankInExport = true}},
        }};
    const DepositInterestSummary deposit{.mImportedQualifying = 1'500 * MONEY_SCALE,
                                         .mAllowance = 1'000 * MONEY_SCALE,
                                         .mAddOtherBanks = false};
    const OtherInterestReportObr other{
        .mRows = {
            {.mKind = BrokerCashInterest{.mBroker = Broker::InteractiveBrokers},
             .mDate = day("2024-03-04"),
             .mGrossInterest = 80 * MONEY_SCALE,
             .mWithholding = {.mForeignTax = 0, .mBlankInExport = true}},
            {.mKind = BondCoupon{.mIsin = "US0000000005"},
             .mDate = day("2024-03-05"),
             .mGrossInterest = 60 * MONEY_SCALE,
             .mWithholding = {.mForeignTax = 12 * MONEY_SCALE}},
        }};

    const TaxEstimate raw = estimateTax(2024, capital, dividends, deposit, other);
    const TaxEstimate estimate = completeTaxEstimate(raw,
                                                     {ReportStatus::Generated,
                                                      ReportStatus::Generated,
                                                      ReportStatus::Generated,
                                                      ReportStatus::Generated});

    ASSERT_EQ(estimate.mForms.size(), 4U);
    EXPECT_EQ(estimate.mForms[0].mForm, ReportType::CapitalGains);
    EXPECT_EQ(estimate.mForms[0].mTax, std::optional<Money>{1'187'500});
    EXPECT_EQ(estimate.mForms[1].mForm, ReportType::Dividends);
    EXPECT_EQ(estimate.mForms[1].mTax, std::optional<Money>{400'000});
    EXPECT_EQ(estimate.mForms[2].mForm, ReportType::BankDepositInterest);
    EXPECT_EQ(estimate.mForms[2].mTax, std::optional<Money>{1'250'000});
    EXPECT_EQ(estimate.mForms[3].mForm, ReportType::OtherInterest);
    EXPECT_EQ(estimate.mForms[3].mTax, std::optional<Money>{260'000});
    // 118.75 + 40.00 + 125.00 + 26.00 = 309.75.
    EXPECT_EQ(estimate.mTotal, 3'097'500);
    EXPECT_FALSE(estimate.mTotalPartial);
}

TEST(TaxEstimateTotal, FormWithTenEuroIsNotAssessedAndCountsZero) {
    // Doh-KDVP with a tax of exactly 10.00 (not assessed) and Doh-Div 40.00.
    const CapitalGainsReportKDVP capital{
        .mLists = {{
            .mIsin = "US0000000001",
            .mRows = {{.mEntry = KdvpPurchaseRow{.mDate = day("2023-05-02"),
                                                 .mUnits = 10 * UNITS_SCALE,
                                                 .mUnitPrice = 790 * UNIT_PRICE_SCALE}},
                      {.mEntry = KdvpSaleRow{.mDate = day("2024-05-02"),
                                             .mUnits = 10 * UNITS_SCALE,
                                             .mUnitPrice = 810 * UNIT_PRICE_SCALE}}},
        }}};
    const DividendReportDiv dividends{
        .mRows = {{
            .mIsin = "US0000000002",
            .mDate = day("2024-03-06"),
            .mKind = DividendKind::Ordinary,
            .mGrossIncome = 160 * MONEY_SCALE,
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
        }}};

    const TaxEstimate raw = estimateTax(2024, capital, dividends, std::nullopt, {});
    const TaxEstimate estimate = completeTaxEstimate(raw,
                                                     {ReportStatus::Generated,
                                                      ReportStatus::Generated,
                                                      ReportStatus::NoData,
                                                      ReportStatus::NoData});

    ASSERT_EQ(estimate.mForms.size(), 2U);
    EXPECT_TRUE(estimate.mForms[0].mNotAssessed);
    // 160.00 x 25 % = 40.00; the 10.00 of Doh-KDVP counts 0.
    EXPECT_EQ(estimate.mTotal, 400'000);
    EXPECT_FALSE(estimate.mTotalPartial);
}

TEST(TaxEstimateTotal, FormThatNeedsInputHasNoAmountAndMakesTheTotalPartial) {
    const DividendReportDiv dividends{
        .mRows = {{
            .mIsin = "US0000000002",
            .mDate = day("2024-03-06"),
            .mKind = DividendKind::Ordinary,
            .mGrossIncome = 160 * MONEY_SCALE,
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
        }}};
    const OtherInterestReportObr other{
        .mRows = {{
            .mKind = BrokerCashInterest{.mBroker = Broker::InteractiveBrokers},
            .mDate = day("2024-03-04"),
            .mGrossInterest = 80 * MONEY_SCALE,
            .mWithholding = {.mForeignTax = 0, .mBlankInExport = true},
        }}};

    const TaxEstimate raw = estimateTax(2024, {}, dividends, std::nullopt, other);
    const TaxEstimate estimate = completeTaxEstimate(raw,
                                                     {ReportStatus::NoData,
                                                      ReportStatus::NeedsInput,
                                                      ReportStatus::NoData,
                                                      ReportStatus::Generated});

    ASSERT_EQ(estimate.mForms.size(), 2U);
    EXPECT_EQ(estimate.mForms[0].mForm, ReportType::Dividends);
    EXPECT_TRUE(estimate.mForms[0].mIncomplete);
    EXPECT_FALSE(estimate.mForms[0].mTax.has_value());
    EXPECT_EQ(estimate.mForms[1].mForm, ReportType::OtherInterest);
    EXPECT_EQ(estimate.mForms[1].mTax, std::optional<Money>{200'000}); // 80.00 x 25 %
    EXPECT_EQ(estimate.mTotal, 200'000);
    EXPECT_TRUE(estimate.mTotalPartial);
}

TEST(TaxEstimateTotal, FailedFormWithoutRowsHasNoAmount) {
    // The form failed before any row was made, so the stages gave no estimate for it.
    const TaxEstimate raw{.mYearSupported = true};

    const TaxEstimate estimate = completeTaxEstimate(
        raw,
        {ReportStatus::Failed, ReportStatus::NoData, ReportStatus::NoData, ReportStatus::NoData});

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mForm, ReportType::CapitalGains);
    EXPECT_TRUE(estimate.mForms[0].mIncomplete);
    EXPECT_EQ(estimate.mTotal, 0);
    EXPECT_TRUE(estimate.mTotalPartial);
}

TEST(TaxEstimateTotal, GeneratedFormWithoutAnEstimateIsIncomplete) {
    const TaxEstimate raw{.mYearSupported = true};

    const TaxEstimate estimate = completeTaxEstimate(raw,
                                                     {ReportStatus::NoData,
                                                      ReportStatus::Generated,
                                                      ReportStatus::NoData,
                                                      ReportStatus::NoData});

    ASSERT_EQ(estimate.mForms.size(), 1U);
    EXPECT_EQ(estimate.mForms[0].mForm, ReportType::Dividends);
    EXPECT_TRUE(estimate.mForms[0].mIncomplete);
    EXPECT_TRUE(estimate.mTotalPartial);
}

TEST(TaxEstimateTotal, AllFormsWithoutDataGiveNoForm) {
    const TaxEstimate raw{.mYearSupported = true};

    const TaxEstimate estimate = completeTaxEstimate(
        raw,
        {ReportStatus::NoData, ReportStatus::NoData, ReportStatus::NoData, ReportStatus::NoData});

    EXPECT_TRUE(estimate.mYearSupported);
    EXPECT_TRUE(estimate.mForms.empty());
    EXPECT_EQ(estimate.mTotal, 0);
    EXPECT_FALSE(estimate.mTotalPartial);
}

TEST(TaxEstimateTotal, UnsupportedYearStaysEmpty) {
    const TaxEstimate raw{.mYearSupported = false};

    const TaxEstimate estimate = completeTaxEstimate(raw,
                                                     {ReportStatus::Generated,
                                                      ReportStatus::Generated,
                                                      ReportStatus::Generated,
                                                      ReportStatus::Generated});

    EXPECT_FALSE(estimate.mYearSupported);
    EXPECT_TRUE(estimate.mForms.empty());
}

TEST(TaxEstimateTotal, UnsupportedYearWithoutAnyDataHasNothingToSay) {
    const TaxEstimate raw{.mYearSupported = false};

    const TaxEstimate estimate = completeTaxEstimate(
        raw,
        {ReportStatus::NoData, ReportStatus::NoData, ReportStatus::NoData, ReportStatus::NoData});

    EXPECT_TRUE(estimate.mYearSupported);
    EXPECT_TRUE(estimate.mForms.empty());
}

TEST(TaxEstimateTotal, TotalThatDoesNotFitIsPartial) {
    // Two forms of 5e18 (Money units) add up to more than the largest Money.
    const TaxEstimate raw{
        .mYearSupported = true,
        .mForms = {{.mForm = ReportType::CapitalGains, .mTax = 5'000'000'000'000'000'000},
                   {.mForm = ReportType::Dividends, .mTax = 5'000'000'000'000'000'000}}};

    const TaxEstimate estimate = completeTaxEstimate(raw,
                                                     {ReportStatus::Generated,
                                                      ReportStatus::Generated,
                                                      ReportStatus::NoData,
                                                      ReportStatus::NoData});

    ASSERT_EQ(estimate.mForms.size(), 2U);
    // The first amount is in the total; the second does not fit and makes the total partial.
    EXPECT_EQ(estimate.mTotal, 5'000'000'000'000'000'000);
    EXPECT_TRUE(estimate.mTotalPartial);
}

} // namespace
