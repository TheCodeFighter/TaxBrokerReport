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
        .mSource =
            SourceReference{
                .mBroker = aBroker,
                .mFilename = SourceFilename::fromPath(aFilename),
                .mSourceRow = aRow,
                .mTransactionId = aTransactionId.empty()
                                      ? std::nullopt
                                      : std::optional<std::string>{aTransactionId},
                .mInputSequence = aSequence,
            },
    };
}

static_assert(std::is_same_v<decltype(CorporateAction{}.mMetadata), EventMetadata>);
static_assert(std::is_same_v<decltype(TradeTransaction{}.mMetadata), EventMetadata>);
static_assert(std::is_same_v<decltype(DividendTransaction{}.mMetadata), EventMetadata>);
static_assert(std::is_same_v<decltype(InterestTransaction{}.mMetadata), EventMetadata>);
static_assert(std::is_same_v<decltype(BenefitEvent{}.mMetadata), EventMetadata>);
static_assert(std::is_same_v<decltype(PrivateMarketEvent{}.mMetadata), EventMetadata>);
static_assert(std::is_same_v<SourceTimestamp::duration, std::chrono::milliseconds>);

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

TEST(EventMetadataTest, ExactEqualityIncludesEverySourceField) {
    const auto original = makeMetadata("transaction-1", 4, {.mSourceIndex = 1, .mEventIndex = 2});
    auto copy = original;
    EXPECT_EQ(original, copy);

    copy.mSource.mInputSequence.mEventIndex = 3;
    EXPECT_NE(original, copy);
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
    std::sort(events.begin(), events.end(), EventMetadataChronologicalLess{});

    const std::array expectedIds{"date", "timestamp-1", "timestamp-2", "fallback-1", "fallback-2"};
    for (std::size_t index = 0; index < events.size(); ++index)
    {
        ASSERT_TRUE(events[index].mSource.mTransactionId.has_value());
        EXPECT_EQ(*events[index].mSource.mTransactionId, expectedIds[index]);
    }
}

TEST(EventMetadataTest, StableInputSequenceBreaksOtherwiseIdenticalTies) {
    const auto first = makeMetadata("same", 2, {.mSourceIndex = 0, .mEventIndex = 1});
    const auto second = makeMetadata("same", 2, {.mSourceIndex = 1, .mEventIndex = 0});
    const EventMetadataChronologicalLess less;

    EXPECT_TRUE(less(first, second));
    EXPECT_FALSE(less(second, first));
    EXPECT_FALSE(less(first, first));
}

} // namespace
