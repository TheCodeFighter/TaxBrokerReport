#include "taxbroker/split_ratio.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace {
using namespace taxbroker;

TEST(SplitRatioTest, ReducesTheEnteredNumbers) {
    // 20 new for 10 old is 2 new for 1 old; 6 for 4 is 3 for 2.
    EXPECT_EQ(makeSplitRatio(20, 10), (SplitRatio{.mNewShares = 2, .mOldShares = 1}));
    EXPECT_EQ(makeSplitRatio(6, 4), (SplitRatio{.mNewShares = 3, .mOldShares = 2}));
    // Already reduced: a ten-for-one split and a one-for-ten reverse split.
    EXPECT_EQ(makeSplitRatio(10, 1), (SplitRatio{.mNewShares = 10, .mOldShares = 1}));
    EXPECT_EQ(makeSplitRatio(1, 10), (SplitRatio{.mNewShares = 1, .mOldShares = 10}));
}

TEST(SplitRatioTest, RejectsEqualNumbers) {
    // The position would not change.
    EXPECT_EQ(makeSplitRatio(5, 5), std::nullopt);
    EXPECT_EQ(makeSplitRatio(1, 1), std::nullopt);
}

TEST(SplitRatioTest, RejectsANumberThatIsNotAboveZero) {
    EXPECT_EQ(makeSplitRatio(0, 1), std::nullopt);
    EXPECT_EQ(makeSplitRatio(1, 0), std::nullopt);
    EXPECT_EQ(makeSplitRatio(-2, 1), std::nullopt);
    EXPECT_EQ(makeSplitRatio(2, -1), std::nullopt);
}

TEST(SplitRatioTest, FindsTheRatioFromTheSharesBeforeAndAfter) {
    // 0.52 before and 5.20 after: 520000000 / 52000000 reduces to 10 / 1.
    EXPECT_EQ(splitRatioFromShareCounts(52'000'000, 520'000'000),
              (SplitRatio{.mNewShares = 10, .mOldShares = 1}));
    // 3 before and 6 after.
    EXPECT_EQ(splitRatioFromShareCounts(300'000'000, 600'000'000),
              (SplitRatio{.mNewShares = 2, .mOldShares = 1}));
    // 10 before and 1 after: a reverse split.
    EXPECT_EQ(splitRatioFromShareCounts(1'000'000'000, 100'000'000),
              (SplitRatio{.mNewShares = 1, .mOldShares = 10}));
}

TEST(SplitRatioTest, RejectsShareCountsThatDoNotDescribeASplit) {
    // 2 before and 2 after.
    EXPECT_EQ(splitRatioFromShareCounts(200'000'000, 200'000'000), std::nullopt);
    // 0 before and 1 after.
    EXPECT_EQ(splitRatioFromShareCounts(0, 100'000'000), std::nullopt);
}

TEST(SplitRatioTest, ARoundedShareCountGivesAnUnusualRatio) {
    // 0.52 before and 5.20000001 after: 520000001 / 52000000, nothing to reduce.
    const auto ratio = splitRatioFromShareCounts(52'000'000, 520'000'001);

    ASSERT_TRUE(ratio.has_value());
    EXPECT_EQ(*ratio, (SplitRatio{.mNewShares = 520'000'001, .mOldShares = 52'000'000}));
    EXPECT_TRUE(isUnusualSplitRatio(*ratio));
}

TEST(SplitRatioTest, ARatioAbove1000IsUnusual) {
    EXPECT_FALSE(isUnusualSplitRatio({.mNewShares = 1000, .mOldShares = 1}));
    EXPECT_FALSE(isUnusualSplitRatio({.mNewShares = 1, .mOldShares = 1000}));
    EXPECT_TRUE(isUnusualSplitRatio({.mNewShares = 1001, .mOldShares = 1}));
    EXPECT_TRUE(isUnusualSplitRatio({.mNewShares = 1, .mOldShares = 1001}));
}

TEST(SplitRatioTest, PreviewsThePositionAfterTheAction) {
    // fifo.md: 3 units with 2 / 1 are 6 units; 6 units with 1 / 3 are 2 units.
    EXPECT_EQ(previewSplitUnits(300'000'000, {.mNewShares = 2, .mOldShares = 1}), 600'000'000);
    EXPECT_EQ(previewSplitUnits(600'000'000, {.mNewShares = 1, .mOldShares = 3}), 200'000'000);
    // fifo.md, "Exact lot adjustment": 1 + 2 units with 5 / 3 are 5 units.
    EXPECT_EQ(previewSplitUnits(300'000'000, {.mNewShares = 5, .mOldShares = 3}), 500'000'000);
    // A ten-for-one split of 0.52 units.
    EXPECT_EQ(previewSplitUnits(52'000'000, {.mNewShares = 10, .mOldShares = 1}), 520'000'000);
}

TEST(SplitRatioTest, RoundsThePreviewOnceToEightDecimals) {
    // 1 unit with 1 / 3 is exactly 0.333333333...; the ninth decimal is 3, so it rounds down.
    EXPECT_EQ(previewSplitUnits(100'000'000, {.mNewShares = 1, .mOldShares = 3}), 33'333'333);
    // 1 unit with 2 / 3 is exactly 0.666666666...; the ninth decimal is 6, so it rounds up.
    EXPECT_EQ(previewSplitUnits(100'000'000, {.mNewShares = 2, .mOldShares = 3}), 66'666'667);
    // 0.00000001 units with 1 / 2 is exactly 0.000000005, a half: away from zero.
    EXPECT_EQ(previewSplitUnits(1, {.mNewShares = 1, .mOldShares = 2}), 1);
}

TEST(SplitRatioTest, APreviewThatDoesNotFitUnitsIsEmpty) {
    // The largest position, 92233720368.54775807 units, doubled does not fit.
    EXPECT_EQ(previewSplitUnits(9'223'372'036'854'775'807, {.mNewShares = 2, .mOldShares = 1}),
              std::nullopt);
    // 0.00000001 units with 1 / 3 is 0.0000000033...: it rounds to 0, which is no position.
    EXPECT_EQ(previewSplitUnits(1, {.mNewShares = 1, .mOldShares = 3}), std::nullopt);
}

} // namespace
