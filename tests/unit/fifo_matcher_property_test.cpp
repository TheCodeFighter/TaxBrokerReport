#include "../support/property_random.hpp"
#include "processors/fifo_matcher.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace taxbroker;

constexpr std::uint64_t PROPERTY_SEED = 20261101;
constexpr int PROPERTY_ITERATIONS = 200;

/// Twelve distinct dates in 2023 and 2024, in order. Trade k is dated kDates[k].
constexpr std::array kDates{std::string_view{"2023-01-10"},
                            std::string_view{"2023-03-10"},
                            std::string_view{"2023-05-10"},
                            std::string_view{"2023-07-10"},
                            std::string_view{"2023-09-10"},
                            std::string_view{"2023-11-10"},
                            std::string_view{"2024-01-10"},
                            std::string_view{"2024-03-10"},
                            std::string_view{"2024-05-10"},
                            std::string_view{"2024-07-10"},
                            std::string_view{"2024-09-10"},
                            std::string_view{"2024-11-10"}};

struct TradeSpec {
    TradeSide mSide{};
    Units mUnits{};
};

/// Runs the matcher for 2024 on one ISIN whose trades are dated kDates[0], kDates[1], ...
FifoMatchingResult matchTrades(const std::vector<TradeSpec>& aTrades) {
    StatementMergeResult merged;
    TradeInstrument instrument{.mName = "Synthetic Share", .mIsin = "XX0000000001"};

    for (std::size_t index = 0; index < aTrades.size(); ++index)
    {
        instrument.mTransactions.push_back(TradeTransaction{
            .mMetadata = {.mTaxDate = *parseCalendarDate(kDates[index])},
            .mTradeSide = aTrades[index].mSide,
            .mUnitPrice = 1'000'000'000, // EUR 10
            .mUnits = aTrades[index].mUnits,
            .mCurrency = Currency::EUR,
        });
        merged.mStatement.mChronologicalOrder.push_back({StatementEventKind::Trade, 0, index});
    }

    merged.mStatement.mPresentation.mTradeInstruments.push_back(std::move(instrument));

    const auto prepared = TaxProcessor{}.prepareReportingYear({2024, merged});

    return FifoMatcher::match(merged.mStatement, prepared.mLedger.value());
}

/// The invariant: stock never goes negative without an error. Its own running total finds the
/// first sale that needs more units than are held. Without such a sale the pool matches and its
/// open lots hold the running total. With one, the pool stops at that sale with one
/// incomplete_history error and keeps the lots from before it. No lot has zero or negative units.
std::optional<std::string> stockFailure(const std::vector<TradeSpec>& aTrades,
                                        const FifoMatchingResult& aResult) {
    Units stock = 0;
    std::optional<std::size_t> shortSale;

    for (std::size_t index = 0; index < aTrades.size() && !shortSale; ++index)
    {
        if (aTrades[index].mSide == TradeSide::Buy)
        {
            stock += aTrades[index].mUnits;
        }
        else if (aTrades[index].mUnits > stock)
        {
            shortSale = index;
        }
        else
        {
            stock -= aTrades[index].mUnits;
        }
    }

    if (aResult.mIsins.size() != 1)
    {
        return "the result does not hold exactly one ISIN";
    }

    const auto& pool = aResult.mIsins.front();
    Units openUnits = 0;

    for (const auto& lot : pool.mOpenLots)
    {
        if (lot.mState.mRemainingUnits <= 0)
        {
            return "an open lot holds no positive units";
        }

        openUnits += lot.mState.mRemainingUnits;
    }

    if (openUnits != stock)
    {
        return "the open units differ from the running stock";
    }

    const auto errorCount = static_cast<std::size_t>(
        std::count_if(aResult.mDiagnostics.begin(),
                      aResult.mDiagnostics.end(),
                      [](const ProcessingDiagnostic& aDiagnostic) {
                          return aDiagnostic.mCode == ProcessingDiagnosticCode::IncompleteHistory;
                      }));

    if (!shortSale)
    {
        if (pool.mStatus != FifoIsinStatus::Matched || !aResult.mDiagnostics.empty())
        {
            return "a history without a short sale did not match cleanly";
        }

        return std::nullopt;
    }

    if (pool.mStatus != FifoIsinStatus::InsufficientUnits)
    {
        return "a short sale did not stop the pool";
    }

    if (pool.mStoppedAt != StatementEventReference{StatementEventKind::Trade, 0, *shortSale})
    {
        return "the pool stopped at another event than the short sale";
    }

    if (pool.mAvailableUnits != stock)
    {
        return "the available units differ from the running stock";
    }

    if (errorCount != 1)
    {
        return "a short sale did not give exactly one incomplete_history error";
    }

    return std::nullopt;
}

TEST(FifoMatcherPropertyTest, ABuyOf2AndASaleOf3StopsWithAnErrorAndKeepsThe2Units) {
    const std::vector<TradeSpec> trades{
        {.mSide = TradeSide::Buy, .mUnits = 200'000'000},  // 2 units
        {.mSide = TradeSide::Sell, .mUnits = 300'000'000}, // 3 units
    };

    const auto result = matchTrades(trades);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_EQ(result.mIsins.front().mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(result.mIsins.front().mAvailableUnits, 200'000'000); // 2 units were held
    ASSERT_EQ(result.mIsins.front().mOpenLots.size(), 1U);
    EXPECT_EQ(result.mIsins.front().mOpenLots.front().mState.mRemainingUnits, 200'000'000);
    EXPECT_EQ(stockFailure(trades, result), std::nullopt);
}

TEST(FifoMatcherPropertyTest, ABuyOf3AndASaleOf2LeaveOneUnitWithoutAnError) {
    const std::vector<TradeSpec> trades{
        {.mSide = TradeSide::Buy, .mUnits = 300'000'000},  // 3 units
        {.mSide = TradeSide::Sell, .mUnits = 200'000'000}, // 2 units
    };

    const auto result = matchTrades(trades);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_EQ(result.mIsins.front().mStatus, FifoIsinStatus::Matched);
    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mIsins.front().mOpenLots.size(), 1U);
    EXPECT_EQ(result.mIsins.front().mOpenLots.front().mState.mRemainingUnits, 100'000'000);
    EXPECT_EQ(stockFailure(trades, result), std::nullopt);
}

TEST(FifoMatcherPropertyTest, StockNeverGoesNegativeWithoutAnError) {
    std::size_t casesWithAShortSale = 0;
    std::size_t casesWithoutAShortSale = 0;

    for (int iteration = 0; iteration < PROPERTY_ITERATIONS; ++iteration)
    {
        const auto seed = PROPERTY_SEED + static_cast<std::uint64_t>(iteration);
        SCOPED_TRACE("seed " + std::to_string(seed));
        test::PropertyRandom random{seed};

        std::vector<TradeSpec> trades;
        const auto tradeCount = static_cast<std::size_t>(random.between(2, 12));

        for (std::size_t index = 0; index < tradeCount; ++index)
        {
            // A buy in two cases out of three, with 1 to 5 whole units.
            trades.push_back({
                .mSide = random.below(3) == 0 ? TradeSide::Sell : TradeSide::Buy,
                .mUnits = random.between(1, 5) * 100'000'000,
            });
        }

        const auto result = matchTrades(trades);

        EXPECT_EQ(stockFailure(trades, result), std::nullopt);

        if (!result.mDiagnostics.empty())
        {
            ++casesWithAShortSale;
        }
        else
        {
            ++casesWithoutAShortSale;
        }
    }

    // Both outcomes must occur, or the property would test only one branch.
    EXPECT_GT(casesWithAShortSale, 0U);
    EXPECT_GT(casesWithoutAShortSale, 0U);
}

TEST(FifoMatcherPropertyTest, TheCheckReportsAPoolThatWentNegativeWithoutAnError) {
    const std::vector<TradeSpec> trades{
        {.mSide = TradeSide::Buy, .mUnits = 200'000'000},  // 2 units
        {.mSide = TradeSide::Sell, .mUnits = 300'000'000}, // 3 units
    };

    // A broken matcher that sells 3 of the 2 units: the pool matches and holds -1 unit.
    FifoMatchingResult broken;
    broken.mIsins.push_back(FifoIsinResult{
        .mIsin = "XX0000000001",
        .mStatus = FifoIsinStatus::Matched,
        .mOpenLots = {FifoLot{.mState = {.mRemainingUnits = -100'000'000}}},
    });

    EXPECT_EQ(stockFailure(trades, broken), "an open lot holds no positive units");
}

TEST(FifoMatcherPropertyTest, TheCheckReportsAShortSaleThatWasAppliedPartly) {
    const std::vector<TradeSpec> trades{
        {.mSide = TradeSide::Buy, .mUnits = 200'000'000},  // 2 units
        {.mSide = TradeSide::Sell, .mUnits = 300'000'000}, // 3 units
    };

    // A broken matcher that takes the 2 units it has, stops, and leaves no lot behind.
    FifoMatchingResult broken;
    broken.mIsins.push_back(FifoIsinResult{
        .mIsin = "XX0000000001",
        .mStatus = FifoIsinStatus::InsufficientUnits,
        .mStoppedAt = StatementEventReference{StatementEventKind::Trade, 0, 1},
        .mAvailableUnits = 0,
    });

    EXPECT_EQ(stockFailure(trades, broken), "the open units differ from the running stock");
}

TEST(FifoMatcherPropertyTest, TheCheckReportsAShortSaleWithoutAnError) {
    const std::vector<TradeSpec> trades{
        {.mSide = TradeSide::Buy, .mUnits = 200'000'000},  // 2 units
        {.mSide = TradeSide::Sell, .mUnits = 300'000'000}, // 3 units
    };

    // A broken matcher that keeps the 2 units but reports the history as complete.
    FifoMatchingResult broken;
    broken.mIsins.push_back(FifoIsinResult{
        .mIsin = "XX0000000001",
        .mStatus = FifoIsinStatus::Matched,
        .mOpenLots = {FifoLot{.mState = {.mRemainingUnits = 200'000'000}}},
    });

    EXPECT_EQ(stockFailure(trades, broken), "a short sale did not stop the pool");
}

TEST(FifoMatcherPropertyTest, TheCheckReportsAResultWithoutThePool) {
    const std::vector<TradeSpec> trades{
        {.mSide = TradeSide::Buy, .mUnits = 200'000'000}, // 2 units
    };

    // A broken matcher that returns no pool at all.
    const FifoMatchingResult broken;

    EXPECT_EQ(stockFailure(trades, broken), "the result does not hold exactly one ISIN");
}

TEST(FifoMatcherPropertyTest, TheCheckReportsACleanHistoryWithAnError) {
    const std::vector<TradeSpec> trades{
        {.mSide = TradeSide::Buy, .mUnits = 300'000'000},  // 3 units
        {.mSide = TradeSide::Sell, .mUnits = 200'000'000}, // 2 units
    };

    // A broken matcher that keeps the right 1 unit but adds an incomplete_history error.
    FifoMatchingResult broken;
    broken.mIsins.push_back(FifoIsinResult{
        .mIsin = "XX0000000001",
        .mStatus = FifoIsinStatus::Matched,
        .mOpenLots = {FifoLot{.mState = {.mRemainingUnits = 100'000'000}}},
    });
    broken.mDiagnostics.push_back(
        ProcessingDiagnostic{.mCode = ProcessingDiagnosticCode::IncompleteHistory});

    EXPECT_EQ(stockFailure(trades, broken), "a history without a short sale did not match cleanly");
}

TEST(FifoMatcherPropertyTest, TheCheckReportsAPoolStoppedAtAnotherEvent) {
    const std::vector<TradeSpec> trades{
        {.mSide = TradeSide::Buy, .mUnits = 200'000'000},  // 2 units
        {.mSide = TradeSide::Sell, .mUnits = 300'000'000}, // 3 units
    };

    // A broken matcher that stops at the buy (trade 0) instead of the short sale (trade 1).
    FifoMatchingResult broken;
    broken.mIsins.push_back(FifoIsinResult{
        .mIsin = "XX0000000001",
        .mStatus = FifoIsinStatus::InsufficientUnits,
        .mStoppedAt = StatementEventReference{StatementEventKind::Trade, 0, 0},
        .mAvailableUnits = 200'000'000,
        .mOpenLots = {FifoLot{.mState = {.mRemainingUnits = 200'000'000}}},
    });
    broken.mDiagnostics.push_back(
        ProcessingDiagnostic{.mCode = ProcessingDiagnosticCode::IncompleteHistory});

    EXPECT_EQ(stockFailure(trades, broken),
              "the pool stopped at another event than the short sale");
}

TEST(FifoMatcherPropertyTest, TheCheckReportsWrongAvailableUnits) {
    const std::vector<TradeSpec> trades{
        {.mSide = TradeSide::Buy, .mUnits = 200'000'000},  // 2 units
        {.mSide = TradeSide::Sell, .mUnits = 300'000'000}, // 3 units
    };

    // A broken matcher that keeps the 2 units but says only 1 unit was available.
    FifoMatchingResult broken;
    broken.mIsins.push_back(FifoIsinResult{
        .mIsin = "XX0000000001",
        .mStatus = FifoIsinStatus::InsufficientUnits,
        .mStoppedAt = StatementEventReference{StatementEventKind::Trade, 0, 1},
        .mAvailableUnits = 100'000'000,
        .mOpenLots = {FifoLot{.mState = {.mRemainingUnits = 200'000'000}}},
    });
    broken.mDiagnostics.push_back(
        ProcessingDiagnostic{.mCode = ProcessingDiagnosticCode::IncompleteHistory});

    EXPECT_EQ(stockFailure(trades, broken), "the available units differ from the running stock");
}

TEST(FifoMatcherPropertyTest, TheCheckReportsAStoppedPoolWithoutAnIncompleteHistoryError) {
    const std::vector<TradeSpec> trades{
        {.mSide = TradeSide::Buy, .mUnits = 200'000'000},  // 2 units
        {.mSide = TradeSide::Sell, .mUnits = 300'000'000}, // 3 units
    };

    // A broken matcher that stops correctly at the short sale but gives no error.
    FifoMatchingResult broken;
    broken.mIsins.push_back(FifoIsinResult{
        .mIsin = "XX0000000001",
        .mStatus = FifoIsinStatus::InsufficientUnits,
        .mStoppedAt = StatementEventReference{StatementEventKind::Trade, 0, 1},
        .mAvailableUnits = 200'000'000,
        .mOpenLots = {FifoLot{.mState = {.mRemainingUnits = 200'000'000}}},
    });

    EXPECT_EQ(stockFailure(trades, broken),
              "a short sale did not give exactly one incomplete_history error");
}

} // namespace
