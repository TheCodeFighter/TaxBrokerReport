#include "taxbroker/event_metadata.hpp"
#include "taxbroker/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using namespace taxbroker;

Date makeDate(int aYear, unsigned aMonth, unsigned aDay) {
    const auto calendarDate =
        std::chrono::year{aYear} / std::chrono::month{aMonth} / std::chrono::day{aDay};
    return Date{std::chrono::sys_days{calendarDate}.time_since_epoch()};
}

SourceTimestamp
makeTimestamp(int aYear, unsigned aMonth, unsigned aDay, int aHour, int aMinute = 0) {
    const auto calendarDate =
        std::chrono::year{aYear} / std::chrono::month{aMonth} / std::chrono::day{aDay};
    return SourceTimestamp{std::chrono::sys_days{calendarDate}.time_since_epoch() +
                           std::chrono::hours{aHour} + std::chrono::minutes{aMinute}};
}

EventMetadata makeMetadata(std::string_view aTransactionId,
                           std::size_t aRow,
                           StableInputSequence aSequence,
                           std::optional<SourceTimestamp> aTimestamp = std::nullopt,
                           Broker aBroker = Broker::TradeRepublic,
                           std::string_view aFilename = "export.csv",
                           Date aDate = makeDate(2024, 1, 15)) {
    return EventMetadata{
        .mTaxDate = aDate,
        .mSourceTimestamp = aTimestamp,
        .mSources = {SourceReference{
            .mBroker = aBroker,
            .mFilename = SourceFilename::fromPath(aFilename),
            .mSourceRow = aRow,
            .mTransactionId =
                aTransactionId.empty() ? std::nullopt : std::optional<std::string>{aTransactionId},
            .mInputSequence = aSequence,
        }},
    };
}

static_assert(std::is_same_v<decltype(CorporateAction{}.mMetadata), EventMetadata>);
static_assert(std::is_same_v<decltype(TradeTransaction{}.mMetadata), EventMetadata>);
static_assert(std::is_same_v<decltype(DividendTransaction{}.mMetadata), EventMetadata>);
static_assert(std::is_same_v<decltype(InterestTransaction{}.mMetadata), EventMetadata>);
static_assert(std::is_same_v<decltype(BenefitEvent{}.mMetadata), EventMetadata>);
static_assert(std::is_same_v<decltype(PrivateMarketEvent{}.mMetadata), EventMetadata>);
static_assert(std::is_same_v<SourceTimestamp::duration, std::chrono::milliseconds>);

template <typename Event>
concept HasAdHocTransactionId = requires(Event aEvent) { aEvent.mTransactionId; };

static_assert(!HasAdHocTransactionId<CorporateAction>);
static_assert(!HasAdHocTransactionId<TradeTransaction>);
static_assert(!HasAdHocTransactionId<DividendTransaction>);
static_assert(!HasAdHocTransactionId<InterestTransaction>);
static_assert(!HasAdHocTransactionId<BenefitEvent>);
static_assert(!HasAdHocTransactionId<PrivateMarketEvent>);

TEST(SourceFilenameTest, RetainsOnlyTheBasenameAcrossPathStyles) {
    EXPECT_EQ(SourceFilename::fromPath("/private/user/export.csv").value(), "export.csv");
    EXPECT_EQ(SourceFilename::fromPath(R"(C:\Users\person\export.csv)").value(), "export.csv");
    EXPECT_EQ(SourceFilename::fromPath("export.csv").value(), "export.csv");
}

TEST(SourceFilenameTest, RejectsPathsWithoutABasename) {
    EXPECT_THROW((void)SourceFilename::fromPath("/private/user/"), std::invalid_argument);
    EXPECT_THROW((void)SourceFilename::fromPath(".."), std::invalid_argument);
    EXPECT_THROW((void)SourceFilename::fromPath(""), std::invalid_argument);
}

TEST(EventMetadataTest, ExactEqualityIncludesEveryMetadataField) {
    const auto original = makeMetadata("transaction-1",
                                       4,
                                       {.mSourceIndex = 1, .mEventIndex = 2},
                                       makeTimestamp(2024, 1, 15, 10));
    EXPECT_EQ(original, original);

    const auto expectDifferent = [&original](auto aMutate) {
        auto changed = original;
        aMutate(changed);
        EXPECT_NE(original, changed);
    };

    expectDifferent([](EventMetadata& aMetadata) { aMetadata.mTaxDate = makeDate(2024, 1, 16); });
    expectDifferent([](EventMetadata& aMetadata) {
        aMetadata.mSourceTimestamp = makeTimestamp(2024, 1, 15, 11);
    });
    expectDifferent([](EventMetadata& aMetadata) {
        aMetadata.mOrderingTimestamp = makeTimestamp(2024, 1, 15, 9);
    });
    expectDifferent([](EventMetadata& aMetadata) {
        aMetadata.mSources.front().mBroker = Broker::InteractiveBrokers;
    });
    expectDifferent([](EventMetadata& aMetadata) {
        aMetadata.mSources.front().mFilename = SourceFilename::fromPath("other.csv");
    });
    expectDifferent([](EventMetadata& aMetadata) { aMetadata.mSources.front().mSourceRow = 5; });
    expectDifferent([](EventMetadata& aMetadata) {
        aMetadata.mSources.front().mTransactionId = "transaction-2";
    });
    expectDifferent([](EventMetadata& aMetadata) {
        aMetadata.mSources.front().mInputSequence.mSourceIndex = 2;
    });
    expectDifferent([](EventMetadata& aMetadata) {
        aMetadata.mSources.front().mInputSequence.mEventIndex = 3;
    });
}

TEST(EventMetadataTest, StableSourceOrderUsesEveryDeterministicTieBreaker) {
    const auto metadata = makeMetadata("transaction-1", 4, {.mSourceIndex = 1, .mEventIndex = 2});
    const auto base = primarySource(metadata);
    const StableSourceOrder order;

    const auto expectOrderedAfter = [&](auto aMutate) {
        auto later = base;
        aMutate(later);
        EXPECT_TRUE(order(base, later));
        EXPECT_FALSE(order(later, base));
    };

    expectOrderedAfter([](SourceReference& aSource) { ++aSource.mInputSequence.mEventIndex; });
    expectOrderedAfter([](SourceReference& aSource) {
        aSource.mFilename = SourceFilename::fromPath("z-export.csv");
    });
    expectOrderedAfter([](SourceReference& aSource) { ++aSource.mSourceRow; });
    expectOrderedAfter(
        [](SourceReference& aSource) { aSource.mBroker = Broker::InteractiveBrokers; });
    expectOrderedAfter([](SourceReference& aSource) { aSource.mTransactionId = "transaction-2"; });

    EXPECT_FALSE(order(base, base));
}

TEST(EventMetadataTest, EmptyMetadataHasNoPrimarySourceOrTransactionIdentity) {
    const EventMetadata metadata;

    EXPECT_THROW((void)primarySource(metadata), std::logic_error);
    EXPECT_FALSE(transactionIdentity(metadata).has_value());
}

TEST(EventMetadataTest, TransactionIdentityIsScopedByBrokerAndIgnoresSourceLocation) {
    const auto first = makeMetadata("shared-id", 2, {.mSourceIndex = 0, .mEventIndex = 0});
    const auto overlap = makeMetadata("shared-id",
                                      99,
                                      {.mSourceIndex = 1, .mEventIndex = 10},
                                      std::nullopt,
                                      Broker::TradeRepublic,
                                      "overlap.csv");
    const auto otherBroker = makeMetadata("shared-id",
                                          2,
                                          {.mSourceIndex = 2, .mEventIndex = 0},
                                          std::nullopt,
                                          Broker::InteractiveBrokers);
    const auto withoutId = makeMetadata("", 2, {.mSourceIndex = 3, .mEventIndex = 0});

    EXPECT_TRUE(sameTransactionIdentity(first, overlap));
    EXPECT_FALSE(sameTransactionIdentity(first, otherBroker));
    EXPECT_FALSE(sameTransactionIdentity(first, withoutId));
    EXPECT_FALSE(transactionIdentity(withoutId).has_value());
}

TEST(EventMetadataTest, ChronologicalComparisonUsesTimestampThenInputSequence) {
    const auto earlyDate = makeMetadata("date",
                                        8,
                                        {.mSourceIndex = 3, .mEventIndex = 8},
                                        std::nullopt,
                                        Broker::TradeRepublic,
                                        "z.csv",
                                        makeDate(2024, 1, 14));
    const auto earlyTimestamp = makeMetadata("timestamp-1",
                                             8,
                                             {.mSourceIndex = 3, .mEventIndex = 8},
                                             makeTimestamp(2024, 1, 15, 9));
    const auto lateTimestamp = makeMetadata("timestamp-2",
                                            7,
                                            {.mSourceIndex = 2, .mEventIndex = 7},
                                            makeTimestamp(2024, 1, 15, 10));
    const auto noTimestampFirst =
        makeMetadata("fallback-1", 4, {.mSourceIndex = 0, .mEventIndex = 4});
    const auto noTimestampSecond =
        makeMetadata("fallback-2", 3, {.mSourceIndex = 1, .mEventIndex = 3});

    std::vector<EventMetadata> events{
        noTimestampSecond,
        lateTimestamp,
        earlyDate,
        noTimestampFirst,
        earlyTimestamp,
    };
    std::sort(events.begin(), events.end(), ChronologicalEventOrder{});

    const std::array expectedIds{"date", "timestamp-1", "timestamp-2", "fallback-1", "fallback-2"};
    for (std::size_t index = 0; index < events.size(); ++index)
    {
        const auto& source = primarySource(events[index]);
        ASSERT_TRUE(source.mTransactionId.has_value());
        EXPECT_EQ(*source.mTransactionId, expectedIds[index]);
    }
}

TEST(EventMetadataTest, StableInputSequenceBreaksOtherwiseIdenticalTies) {
    const auto first = makeMetadata("same", 2, {.mSourceIndex = 0, .mEventIndex = 1});
    const auto second = makeMetadata("same", 2, {.mSourceIndex = 1, .mEventIndex = 0});
    const ChronologicalEventOrder order;

    EXPECT_TRUE(order(first, second));
    EXPECT_FALSE(order(second, first));
    EXPECT_FALSE(order(first, first));
}

TEST(EventMetadataTest, MetadataWithoutSourcesSortsAfterSourcedMetadata) {
    const auto sourced = makeMetadata("transaction-1", 2, {.mSourceIndex = 0, .mEventIndex = 0});
    const EventMetadata withoutSource{
        .mTaxDate = sourced.mTaxDate,
        .mSourceTimestamp = sourced.mSourceTimestamp,
    };
    const ChronologicalEventOrder order;

    EXPECT_TRUE(order(sourced, withoutSource));
    EXPECT_FALSE(order(withoutSource, sourced));
    EXPECT_FALSE(order(withoutSource, withoutSource));
}

TEST(EventMetadataTest, ChronologicalComparisonHasNoEconomicSourceLocationFallbacks) {
    const auto timestamp = makeTimestamp(2024, 1, 15, 8);
    const auto first = makeMetadata("z-id",
                                    99,
                                    {.mSourceIndex = 0, .mEventIndex = 2},
                                    timestamp,
                                    Broker::InteractiveBrokers,
                                    "z.csv");
    const auto second = makeMetadata("a-id",
                                     2,
                                     {.mSourceIndex = 1, .mEventIndex = 0},
                                     timestamp,
                                     Broker::TradeRepublic,
                                     "a.csv");
    auto equalSequence = second;
    equalSequence.mSources.front().mInputSequence = first.mSources.front().mInputSequence;
    const ChronologicalEventOrder order;

    EXPECT_TRUE(order(first, second));
    EXPECT_FALSE(order(second, first));
    EXPECT_FALSE(order(first, equalSequence));
    EXPECT_FALSE(order(equalSequence, first));
}

TEST(EventMetadataTest, OrderingTimestampOverridesSourceTimeOnEitherSideOfComparison) {
    auto assumed = makeMetadata("assumed", 2, {0, 0}, makeTimestamp(2024, 1, 16, 23));
    assumed.mOrderingTimestamp = makeTimestamp(2024, 1, 15, 9);
    const auto before = makeMetadata("before", 2, {1, 0}, makeTimestamp(2024, 1, 15, 8));
    const auto after = makeMetadata("after", 2, {2, 0}, makeTimestamp(2024, 1, 15, 10));
    const ChronologicalEventOrder order;

    EXPECT_TRUE(order(before, assumed));
    EXPECT_FALSE(order(assumed, before));
    EXPECT_TRUE(order(assumed, after));
    EXPECT_FALSE(order(after, assumed));
    EXPECT_FALSE(order(assumed, assumed));
}

TEST(EventMetadataTest, EqualOrderingTimesUseRequestOrderDespiteDifferentSourceTimes) {
    auto first = makeMetadata("first", 2, {0, 0}, makeTimestamp(2024, 1, 17, 23));
    auto second = makeMetadata("second", 2, {1, 0}, makeTimestamp(2024, 1, 16, 8));
    first.mOrderingTimestamp = makeTimestamp(2024, 1, 15, 9);
    second.mOrderingTimestamp = makeTimestamp(2024, 1, 15, 9);
    const ChronologicalEventOrder order;

    // Once ordering times tie, the preserved source times are not another ordering key.
    EXPECT_TRUE(order(first, second));
    EXPECT_FALSE(order(second, first));
}

TEST(EventMetadataTest, TaxDatePrecedesOrderingTimestampAcrossAYearBoundary) {
    auto december = makeMetadata("december",
                                 2,
                                 {1, 0},
                                 makeTimestamp(2024, 1, 1, 23),
                                 Broker::TradeRepublic,
                                 "december.csv",
                                 makeDate(2023, 12, 31));
    december.mOrderingTimestamp = makeTimestamp(2023, 12, 31, 9);
    const auto january = makeMetadata("january",
                                      2,
                                      {0, 0},
                                      makeTimestamp(2024, 1, 1, 8),
                                      Broker::TradeRepublic,
                                      "january.csv",
                                      makeDate(2024, 1, 1));
    const ChronologicalEventOrder order;

    EXPECT_TRUE(order(december, january));
    EXPECT_FALSE(order(january, december));
}

} // namespace
