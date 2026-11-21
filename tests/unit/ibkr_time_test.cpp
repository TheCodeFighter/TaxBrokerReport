#include "parsers/ibkr_time.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using namespace std::chrono;
using taxbroker::SourceTimestamp;
using taxbroker::ibkr::CompactDateTime;
using taxbroker::ibkr::easternToUtc;
using taxbroker::ibkr::parseCompactDate;
using taxbroker::ibkr::parseCompactDateTime;
using taxbroker::ibkr::TimeOfDay;

/// The UTC instant with the given calendar fields, to write the expected values as dates.
SourceTimestamp utc(const int aYear,
                    const unsigned aMonth,
                    const unsigned aDay,
                    const int aHour,
                    const int aMinute,
                    const int aSecond) {
    const sys_seconds seconds = sys_days{year{aYear} / month{aMonth} / day{aDay}} + hours{aHour} +
                                minutes{aMinute} + std::chrono::seconds{aSecond};

    return SourceTimestamp{duration_cast<milliseconds>(seconds.time_since_epoch())};
}

taxbroker::Date date(const int aYear, const unsigned aMonth, const unsigned aDay) {
    return taxbroker::Date{sys_days{year{aYear} / month{aMonth} / day{aDay}}.time_since_epoch()};
}

} // namespace

TEST(IbkrTimeTest, WinterTimeIsFiveHoursBehindUtc) {
    // 2024-03-05 is before the clocks change: EST, UTC-5.
    EXPECT_EQ(easternToUtc(2024, 3, 5, 15, 42, 10), utc(2024, 3, 5, 20, 42, 10));
}

TEST(IbkrTimeTest, SummerTimeIsFourHoursBehindUtc) {
    // 2024-07-01 is in summer time: EDT, UTC-4.
    EXPECT_EQ(easternToUtc(2024, 7, 1, 10, 0, 0), utc(2024, 7, 1, 14, 0, 0));
}

TEST(IbkrTimeTest, LastSecondBeforeTheMarchSwitchIsWinterTime) {
    // The clocks change on Sunday 2024-03-10 at 02:00 (the second Sunday of March).
    EXPECT_EQ(easternToUtc(2024, 3, 10, 1, 59, 59), utc(2024, 3, 10, 6, 59, 59));
}

TEST(IbkrTimeTest, FirstSecondAfterTheMarchSwitchIsSummerTime) {
    EXPECT_EQ(easternToUtc(2024, 3, 10, 3, 0, 0), utc(2024, 3, 10, 7, 0, 0));
}

TEST(IbkrTimeTest, TheHourThatDoesNotExistInMarchGivesNoValue) {
    // The clock jumps from 02:00 to 03:00.
    EXPECT_FALSE(easternToUtc(2024, 3, 10, 2, 0, 0).has_value());
    EXPECT_FALSE(easternToUtc(2024, 3, 10, 2, 30, 0).has_value());
    EXPECT_FALSE(easternToUtc(2024, 3, 10, 2, 59, 59).has_value());
}

TEST(IbkrTimeTest, TheSameHourOnAnotherDayExists) {
    // 02:30 on Saturday 2024-03-09 and on Monday 2024-03-11 are normal times.
    EXPECT_EQ(easternToUtc(2024, 3, 9, 2, 30, 0), utc(2024, 3, 9, 7, 30, 0));
    EXPECT_EQ(easternToUtc(2024, 3, 11, 2, 30, 0), utc(2024, 3, 11, 6, 30, 0));
}

TEST(IbkrTimeTest, TheRepeatedHourInNovemberIsReadAsSummerTime) {
    // The clocks change back on Sunday 2024-11-03 (the first Sunday of November): 01:00 to 01:59
    // happens twice. The first pass (EDT, UTC-4) is used.
    EXPECT_EQ(easternToUtc(2024, 11, 3, 0, 59, 59), utc(2024, 11, 3, 4, 59, 59));
    EXPECT_EQ(easternToUtc(2024, 11, 3, 1, 0, 0), utc(2024, 11, 3, 5, 0, 0));
    EXPECT_EQ(easternToUtc(2024, 11, 3, 1, 30, 0), utc(2024, 11, 3, 5, 30, 0));
    EXPECT_EQ(easternToUtc(2024, 11, 3, 1, 59, 59), utc(2024, 11, 3, 5, 59, 59));
}

TEST(IbkrTimeTest, FromTwoOClockInNovemberItIsWinterTime) {
    EXPECT_EQ(easternToUtc(2024, 11, 3, 2, 0, 0), utc(2024, 11, 3, 7, 0, 0));
    EXPECT_EQ(easternToUtc(2024, 11, 4, 9, 30, 0), utc(2024, 11, 4, 14, 30, 0));
}

TEST(IbkrTimeTest, TheSwitchDaysOfAnotherYearFollowTheSameRule) {
    // 2025: second Sunday of March is the 9th, first Sunday of November is the 2nd.
    EXPECT_EQ(easternToUtc(2025, 3, 9, 1, 59, 59), utc(2025, 3, 9, 6, 59, 59));
    EXPECT_EQ(easternToUtc(2025, 3, 9, 3, 0, 0), utc(2025, 3, 9, 7, 0, 0));
    EXPECT_FALSE(easternToUtc(2025, 3, 9, 2, 15, 0).has_value());
    EXPECT_EQ(easternToUtc(2025, 11, 2, 1, 30, 0), utc(2025, 11, 2, 5, 30, 0));
    EXPECT_EQ(easternToUtc(2025, 11, 2, 2, 0, 0), utc(2025, 11, 2, 7, 0, 0));
}

TEST(IbkrTimeTest, TheRuleOfTheCurrentLawStartsIn2007) {
    // Before 2007 the clocks changed on other days, so no value is made up.
    EXPECT_FALSE(easternToUtc(2006, 7, 1, 10, 0, 0).has_value());
    EXPECT_EQ(easternToUtc(2007, 7, 1, 10, 0, 0), utc(2007, 7, 1, 14, 0, 0));
}

TEST(IbkrTimeTest, ImpossibleFieldsGiveNoValue) {
    EXPECT_FALSE(easternToUtc(2024, 2, 30, 10, 0, 0).has_value()); // no 30 February
    EXPECT_FALSE(easternToUtc(2024, 13, 1, 10, 0, 0).has_value()); // no month 13
    EXPECT_FALSE(easternToUtc(2024, 3, 5, 24, 0, 0).has_value());  // hour 24
    EXPECT_FALSE(easternToUtc(2024, 3, 5, 10, 60, 0).has_value()); // minute 60
    EXPECT_FALSE(easternToUtc(2024, 3, 5, 10, 0, 60).has_value()); // second 60
    EXPECT_FALSE(easternToUtc(2024, 3, 5, -1, 0, 0).has_value());  // negative hour
}

TEST(IbkrTimeTest, CompactDateReadsEightDigits) {
    EXPECT_EQ(parseCompactDate("20240305"), date(2024, 3, 5));
    EXPECT_EQ(parseCompactDate("20240229"), date(2024, 2, 29)); // 2024 is a leap year
}

TEST(IbkrTimeTest, CompactDateRejectsEveryOtherShape) {
    EXPECT_FALSE(parseCompactDate("2024-03-05").has_value()); // dashes
    EXPECT_FALSE(parseCompactDate("2024030").has_value());    // 7 digits
    EXPECT_FALSE(parseCompactDate("202403050").has_value());  // 9 digits
    EXPECT_FALSE(parseCompactDate("20240230").has_value());   // no 30 February
    EXPECT_FALSE(parseCompactDate("20230229").has_value());   // 2023 is not a leap year
    EXPECT_FALSE(parseCompactDate("2024 305").has_value());   // a space
    EXPECT_FALSE(parseCompactDate("2024030a").has_value());   // a letter
    EXPECT_FALSE(parseCompactDate("+2240305").has_value());   // a sign
    EXPECT_FALSE(parseCompactDate("").has_value());
}

TEST(IbkrTimeTest, DateAndTimeReadsTheEasternTimeAsUtc) {
    const auto result = parseCompactDateTime("20240305;154210", TimeOfDay::Required);

    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->mDate, date(2024, 3, 5));
    EXPECT_EQ(result->mTimestamp, utc(2024, 3, 5, 20, 42, 10)); // EST, UTC-5
}

TEST(IbkrTimeTest, AnOvernightTimeKeepsTheDateAsWritten) {
    // The time is on the evening of 5 March; the UTC instant is already on 6 March. The date part
    // stays the one written.
    const auto result = parseCompactDateTime("20240305;203000", TimeOfDay::Required);

    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->mDate, date(2024, 3, 5));
    EXPECT_EQ(result->mTimestamp, utc(2024, 3, 6, 1, 30, 0));
}

TEST(IbkrTimeTest, TheFiveSwitchCasesOfTheContract) {
    const auto beforeMarch = parseCompactDateTime("20240310;015959", TimeOfDay::Required);
    const auto afterMarch = parseCompactDateTime("20240310;030000", TimeOfDay::Required);
    const auto repeatedHour = parseCompactDateTime("20241103;013000", TimeOfDay::Required);
    const auto afterNovember = parseCompactDateTime("20241103;020000", TimeOfDay::Required);
    const auto missingHour = parseCompactDateTime("20240310;023000", TimeOfDay::Required);

    ASSERT_TRUE(beforeMarch.has_value());
    ASSERT_TRUE(afterMarch.has_value());
    ASSERT_TRUE(repeatedHour.has_value());
    ASSERT_TRUE(afterNovember.has_value());

    EXPECT_EQ(beforeMarch->mTimestamp, utc(2024, 3, 10, 6, 59, 59));
    EXPECT_EQ(afterMarch->mTimestamp, utc(2024, 3, 10, 7, 0, 0));
    EXPECT_EQ(repeatedHour->mTimestamp, utc(2024, 11, 3, 5, 30, 0));
    EXPECT_EQ(afterNovember->mTimestamp, utc(2024, 11, 3, 7, 0, 0));
    EXPECT_FALSE(missingHour.has_value());
}

TEST(IbkrTimeTest, ADateWithoutTimeHasNoTimestampWhenTheTimeIsOptional) {
    const auto result = parseCompactDateTime("20240315", TimeOfDay::Optional);

    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->mDate, date(2024, 3, 15));
    EXPECT_FALSE(result->mTimestamp.has_value());
}

TEST(IbkrTimeTest, ADateWithoutTimeIsRejectedWhenTheTimeIsRequired) {
    EXPECT_FALSE(parseCompactDateTime("20240315", TimeOfDay::Required).has_value());
}

TEST(IbkrTimeTest, ATimeIsAlsoAcceptedWhenItIsOptional) {
    const auto result = parseCompactDateTime("20240315;093000", TimeOfDay::Optional);

    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->mDate, date(2024, 3, 15));
    EXPECT_EQ(result->mTimestamp, utc(2024, 3, 15, 13, 30, 0)); // EDT, UTC-4
}

TEST(IbkrTimeTest, DateAndTimeRejectsEveryOtherShape) {
    constexpr auto kRequired = TimeOfDay::Required;

    EXPECT_FALSE(parseCompactDateTime("20240305 154210", kRequired).has_value());  // a space
    EXPECT_FALSE(parseCompactDateTime("20240305T154210", kRequired).has_value());  // a T
    EXPECT_FALSE(parseCompactDateTime("20240305;15421", kRequired).has_value());   // 5 time digits
    EXPECT_FALSE(parseCompactDateTime("20240305;1542100", kRequired).has_value()); // 7 time digits
    EXPECT_FALSE(parseCompactDateTime("20240305;154a10", kRequired).has_value());  // a letter
    EXPECT_FALSE(parseCompactDateTime("20240305;244210", kRequired).has_value());  // hour 24
    EXPECT_FALSE(parseCompactDateTime("20240230;154210", kRequired).has_value());  // no 30 Feb
    EXPECT_FALSE(parseCompactDateTime("2024-03-05;154210", kRequired).has_value());
    EXPECT_FALSE(parseCompactDateTime("", kRequired).has_value());
    EXPECT_FALSE(parseCompactDateTime("", TimeOfDay::Optional).has_value());
    EXPECT_FALSE(parseCompactDateTime("20240230", TimeOfDay::Optional).has_value());
}
