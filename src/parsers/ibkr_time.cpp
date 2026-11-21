#include "parsers/ibkr_time.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <string_view>
#include <system_error>

namespace {

using namespace std::chrono;

/// The year the current US daylight-saving rule starts at.
constexpr int kFirstYearOfCurrentRule = 2007;

constexpr hours kStandardOffset{5}; ///< EST is UTC-5.
constexpr hours kSummerOffset{4};   ///< EDT is UTC-4.

/// The whole text as a number; false for an empty text or any character that is not a digit.
bool readDigits(std::string_view aText, int& aValue) {
    if (aText.empty() || !std::all_of(aText.begin(), aText.end(), [](const char aCharacter) {
            return aCharacter >= '0' && aCharacter <= '9';
        }))
    {
        return false;
    }

    const auto* end = aText.data() + aText.size();
    const auto [parsedEnd, error] = std::from_chars(aText.data(), end, aValue);

    return error == std::errc{} && parsedEnd == end;
}

} // namespace

namespace taxbroker::ibkr {

std::optional<SourceTimestamp> easternToUtc(const int aYear,
                                            const unsigned aMonth,
                                            const unsigned aDay,
                                            const int aHour,
                                            const int aMinute,
                                            const int aSecond) {
    const auto day = year{aYear} / month{aMonth} / std::chrono::day{aDay};

    if (aYear < kFirstYearOfCurrentRule || !day.ok() || aHour < 0 || aHour > 23 || aMinute < 0 ||
        aMinute > 59 || aSecond < 0 || aSecond > 59)
    {
        return std::nullopt;
    }

    // The wall-clock time written as if it were UTC, to compare with the switch times.
    const auto local = sys_days{day} + hours{aHour} + minutes{aMinute} + seconds{aSecond};
    const auto summerStart = sys_days{year{aYear} / March / Sunday[2]} + hours{2};
    const auto summerEnd = sys_days{year{aYear} / November / Sunday[1]} + hours{2};

    // The clock jumps from 02:00 to 03:00: the hour in between never happens.
    if (local >= summerStart && local < summerStart + hours{1})
    {
        return std::nullopt;
    }

    // At the end the clock goes back from 02:00 to 01:00. The repeated hour is read as summer time,
    // so summer time lasts until the wall clock first shows 02:00.
    const bool isSummer = local >= summerStart + hours{1} && local < summerEnd;
    const auto utc = local + (isSummer ? kSummerOffset : kStandardOffset);

    return SourceTimestamp{duration_cast<milliseconds>(utc.time_since_epoch())};
}

std::optional<Date> parseCompactDate(const std::string_view aText) {
    constexpr std::size_t kLength = 8;

    int yearValue{};
    int monthValue{};
    int dayValue{};

    if (aText.size() != kLength || !readDigits(aText.substr(0, 4), yearValue) ||
        !readDigits(aText.substr(4, 2), monthValue) || !readDigits(aText.substr(6, 2), dayValue))
    {
        return std::nullopt;
    }

    const auto day = year{yearValue} / month{static_cast<unsigned>(monthValue)} /
                     std::chrono::day{static_cast<unsigned>(dayValue)};

    if (!day.ok())
    {
        return std::nullopt;
    }

    return Date{sys_days{day}.time_since_epoch()};
}

std::optional<CompactDateTime> parseCompactDateTime(const std::string_view aText,
                                                    const TimeOfDay aTimeOfDay) {
    constexpr std::size_t kDateLength = 8;
    constexpr std::size_t kDateTimeLength = 15; // yyyyMMdd;HHmmss

    if (aText.size() == kDateLength && aTimeOfDay == TimeOfDay::Optional)
    {
        const auto date = parseCompactDate(aText);

        if (!date)
        {
            return std::nullopt;
        }

        return CompactDateTime{.mDate = *date, .mTimestamp = std::nullopt};
    }

    if (aText.size() != kDateTimeLength || aText[kDateLength] != ';')
    {
        return std::nullopt;
    }

    const auto date = parseCompactDate(aText.substr(0, kDateLength));
    int hourValue{};
    int minuteValue{};
    int secondValue{};

    if (!date || !readDigits(aText.substr(9, 2), hourValue) ||
        !readDigits(aText.substr(11, 2), minuteValue) ||
        !readDigits(aText.substr(13, 2), secondValue))
    {
        return std::nullopt;
    }

    const year_month_day day{sys_days{date->time_since_epoch()}};
    const auto timestamp = easternToUtc(static_cast<int>(day.year()),
                                        static_cast<unsigned>(day.month()),
                                        static_cast<unsigned>(day.day()),
                                        hourValue,
                                        minuteValue,
                                        secondValue);

    if (!timestamp)
    {
        return std::nullopt;
    }

    return CompactDateTime{.mDate = *date, .mTimestamp = *timestamp};
}

} // namespace taxbroker::ibkr
