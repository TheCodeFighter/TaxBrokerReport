#include "utils/date_utils.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <string_view>
#include <system_error>

namespace {

bool parseDigits(std::string_view aValue, int& aResult) {
    if (aValue.empty() || !std::all_of(aValue.begin(), aValue.end(), [](char aCharacter) {
            return aCharacter >= '0' && aCharacter <= '9';
        }))
    {
        return false;
    }

    const auto* end = aValue.data() + aValue.size();
    const auto [parsedEnd, error] = std::from_chars(aValue.data(), end, aResult);
    return error == std::errc{} && parsedEnd == end;
}

std::optional<std::chrono::year_month_day> parseYearMonthDay(std::string_view aValue) {
    if (aValue.size() != 10 || aValue[4] != '-' || aValue[7] != '-')
    {
        return std::nullopt;
    }

    int year{};
    int month{};
    int day{};
    if (!parseDigits(aValue.substr(0, 4), year) || !parseDigits(aValue.substr(5, 2), month) ||
        !parseDigits(aValue.substr(8, 2), day))
    {
        return std::nullopt;
    }

    const auto calendarDate = std::chrono::year{year} /
                              std::chrono::month{static_cast<unsigned>(month)} /
                              std::chrono::day{static_cast<unsigned>(day)};
    if (!calendarDate.ok())
    {
        return std::nullopt;
    }

    return calendarDate;
}

} // namespace

namespace taxbroker {

std::optional<Date> parseCalendarDate(std::string_view aValue) {
    const auto calendarDate = parseYearMonthDay(aValue);
    if (!calendarDate)
    {
        return std::nullopt;
    }

    return Date{std::chrono::sys_days{*calendarDate}.time_since_epoch()};
}

std::optional<SourceTimestamp> parseSourceTimestamp(std::string_view aValue) {
    constexpr std::size_t kTimeStart = 11;
    constexpr std::size_t kTimezoneOrFractionPosition = 19;
    if (aValue.size() < 20 || aValue[10] != 'T' || aValue[13] != ':' || aValue[16] != ':')
    {
        return std::nullopt;
    }

    const auto calendarDate = parseYearMonthDay(aValue.substr(0, 10));
    int hour{};
    int minute{};
    int second{};
    if (!calendarDate || !parseDigits(aValue.substr(kTimeStart, 2), hour) ||
        !parseDigits(aValue.substr(kTimeStart + 3, 2), minute) ||
        !parseDigits(aValue.substr(kTimeStart + 6, 2), second) || hour > 23 || minute > 59 ||
        second > 59)
    {
        return std::nullopt;
    }

    std::size_t timezonePosition = kTimezoneOrFractionPosition;
    std::chrono::milliseconds fractional{};
    if (aValue[timezonePosition] == '.')
    {
        const auto fractionalStart = ++timezonePosition;
        while (timezonePosition < aValue.size() && aValue[timezonePosition] >= '0' &&
               aValue[timezonePosition] <= '9')
        {
            ++timezonePosition;
        }

        const auto fractionalDigits = timezonePosition - fractionalStart;
        if (fractionalDigits == 0 || fractionalDigits > 9)
        {
            return std::nullopt;
        }

        const auto retainedDigits = std::min<std::size_t>(fractionalDigits, 3);
        int fractionalValue{};
        if (!parseDigits(aValue.substr(fractionalStart, retainedDigits), fractionalValue))
        {
            return std::nullopt;
        }
        for (std::size_t digit = retainedDigits; digit < 3; ++digit)
        {
            fractionalValue *= 10;
        }
        fractional = std::chrono::milliseconds{fractionalValue};
    }

    std::chrono::minutes utcOffset{};
    if (timezonePosition < aValue.size() && aValue[timezonePosition] == 'Z')
    {
        if (timezonePosition + 1 != aValue.size())
        {
            return std::nullopt;
        }
    }
    else
    {
        if (timezonePosition + 6 != aValue.size() ||
            (aValue[timezonePosition] != '+' && aValue[timezonePosition] != '-') ||
            aValue[timezonePosition + 3] != ':')
        {
            return std::nullopt;
        }

        int offsetHour{};
        int offsetMinute{};
        if (!parseDigits(aValue.substr(timezonePosition + 1, 2), offsetHour) ||
            !parseDigits(aValue.substr(timezonePosition + 4, 2), offsetMinute) || offsetHour > 23 ||
            offsetMinute > 59)
        {
            return std::nullopt;
        }

        utcOffset = std::chrono::hours{offsetHour} + std::chrono::minutes{offsetMinute};
        if (aValue[timezonePosition] == '-')
        {
            utcOffset = -utcOffset;
        }
    }

    return SourceTimestamp{std::chrono::sys_days{*calendarDate}.time_since_epoch() +
                           std::chrono::hours{hour} + std::chrono::minutes{minute} +
                           std::chrono::seconds{second} + fractional - utcOffset};
}

} // namespace taxbroker
