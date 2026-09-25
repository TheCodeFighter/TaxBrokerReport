#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string_view>

namespace {

using namespace taxbroker;

Date makeDate(int aYear, unsigned aMonth, unsigned aDay) {
    const auto calendarDate =
        std::chrono::year{aYear} / std::chrono::month{aMonth} / std::chrono::day{aDay};
    return Date{std::chrono::sys_days{calendarDate}.time_since_epoch()};
}

SourceTimestamp makeTimestamp(int aYear,
                              unsigned aMonth,
                              unsigned aDay,
                              int aHour,
                              int aMinute,
                              int aSecond,
                              int aMillisecond = 0) {
    const auto calendarDate =
        std::chrono::year{aYear} / std::chrono::month{aMonth} / std::chrono::day{aDay};
    return SourceTimestamp{std::chrono::sys_days{calendarDate}.time_since_epoch() +
                           std::chrono::hours{aHour} + std::chrono::minutes{aMinute} +
                           std::chrono::seconds{aSecond} + std::chrono::milliseconds{aMillisecond}};
}

TEST(DateUtilsTest, ParsesValidCalendarDatesIncludingLeapDays) {
    EXPECT_EQ(parseCalendarDate("2024-01-15"), makeDate(2024, 1, 15));
    EXPECT_EQ(parseCalendarDate("2024-02-29"), makeDate(2024, 2, 29));
    EXPECT_EQ(parseCalendarDate("2000-02-29"), makeDate(2000, 2, 29));
    EXPECT_EQ(parseCalendarDate("9999-12-31"), makeDate(9999, 12, 31));
}

TEST(DateUtilsTest, RejectsMalformedAndImpossibleCalendarDates) {
    constexpr std::array<std::string_view, 15> invalidDates{
        "",
        "2024-1-15",
        "2024/01/15",
        "202A-01-15",
        "-001-01-15",
        "2024-01-15 ",
        "10000-01-01",
        "1900-02-29",
        "2023-02-29",
        "2024-02-30",
        "2024-04-31",
        "2024-00-01",
        "2024-13-01",
        "2024-01-00",
        "2024-01-32",
    };

    for (const std::string_view value : invalidDates)
    {
        SCOPED_TRACE(value);
        EXPECT_FALSE(parseCalendarDate(value).has_value());
    }
}

TEST(DateUtilsTest, ParsesUtcTimestampsWithAndWithoutFractionalSeconds) {
    EXPECT_EQ(parseSourceTimestamp("2024-01-15T10:20:30Z"), makeTimestamp(2024, 1, 15, 10, 20, 30));
    EXPECT_EQ(parseSourceTimestamp("2024-01-15T10:20:30.000Z"),
              makeTimestamp(2024, 1, 15, 10, 20, 30));
}

TEST(DateUtilsTest, NormalizesTimezoneOffsetsAcrossUtcDateBoundaries) {
    EXPECT_EQ(parseSourceTimestamp("2024-01-15T00:30:00.123+01:30"),
              makeTimestamp(2024, 1, 14, 23, 0, 0, 123));
    EXPECT_EQ(parseSourceTimestamp("2024-01-15T23:30:00-02:30"),
              makeTimestamp(2024, 1, 16, 2, 0, 0));
    EXPECT_EQ(parseSourceTimestamp("2024-01-15T10:20:30+00:00"),
              makeTimestamp(2024, 1, 15, 10, 20, 30));
}

TEST(DateUtilsTest, PadsAndTruncatesFractionalSecondsToMilliseconds) {
    EXPECT_EQ(parseSourceTimestamp("2024-01-15T10:20:30.1Z"),
              makeTimestamp(2024, 1, 15, 10, 20, 30, 100));
    EXPECT_EQ(parseSourceTimestamp("2024-01-15T10:20:30.12Z"),
              makeTimestamp(2024, 1, 15, 10, 20, 30, 120));
    EXPECT_EQ(parseSourceTimestamp("2024-01-15T10:20:30.123Z"),
              makeTimestamp(2024, 1, 15, 10, 20, 30, 123));
    EXPECT_EQ(parseSourceTimestamp("2024-01-15T10:20:30.123456789Z"),
              makeTimestamp(2024, 1, 15, 10, 20, 30, 123));
    EXPECT_EQ(parseSourceTimestamp("2024-01-15T10:20:30.0009Z"),
              makeTimestamp(2024, 1, 15, 10, 20, 30));
}

TEST(DateUtilsTest, AcceptsValidTimestampComponentBoundaries) {
    EXPECT_EQ(parseSourceTimestamp("2024-02-29T23:59:59.999+23:59"),
              makeTimestamp(2024, 2, 29, 0, 0, 59, 999));
    EXPECT_EQ(parseSourceTimestamp("2024-01-01T00:00:00-23:59"),
              makeTimestamp(2024, 1, 1, 23, 59, 0));
}

TEST(DateUtilsTest, RejectsMalformedTimestamps) {
    constexpr std::array<std::string_view, 10> invalidTimestamps{
        "",
        "2024-01-15T10:20:30",
        "2024-01-15 10:20:30Z",
        "2024-01-15T10-20-30Z",
        "2024-01-15T10:20:30z",
        "2024-01-15T10:20:30+0100",
        "2024-01-15T10:20:30+01:00extra",
        "2024-01-15T10:20:30.Z",
        "2024-01-15T10:20:30.1234567890Z",
        "2024-01-15T-1:20:30Z",
    };

    for (const std::string_view value : invalidTimestamps)
    {
        SCOPED_TRACE(value);
        EXPECT_FALSE(parseSourceTimestamp(value).has_value());
    }
}

TEST(DateUtilsTest, RejectsImpossibleAndOutOfRangeTimestampComponents) {
    constexpr std::array<std::string_view, 7> invalidTimestamps{
        "2023-02-29T10:20:30Z",
        "2024-13-01T10:20:30Z",
        "2024-01-15T24:00:00Z",
        "2024-01-15T23:60:00Z",
        "2024-01-15T23:59:60Z",
        "2024-01-15T10:20:30+24:00",
        "2024-01-15T10:20:30+00:60",
    };

    for (const std::string_view value : invalidTimestamps)
    {
        SCOPED_TRACE(value);
        EXPECT_FALSE(parseSourceTimestamp(value).has_value());
    }
}

} // namespace
