#pragma once

#include "taxbroker/event_metadata.hpp"

#include <optional>
#include <string_view>

namespace taxbroker::ibkr {

/// Converts a wall-clock time in US Eastern time to UTC with the daylight-saving rule in force
/// since 2007 (second Sunday of March to first Sunday of November, at 02:00 local time). The
/// repeated hour of November is read as summer time. A time that does not exist (02:00 to 02:59 on
/// the March day), a date that is not real and a year before 2007 give no value.
[[nodiscard]] std::optional<SourceTimestamp>
easternToUtc(int aYear, unsigned aMonth, unsigned aDay, int aHour, int aMinute, int aSecond);

/// The date from `yyyyMMdd` (`20240305`), or none for any other text or a date that is not real.
[[nodiscard]] std::optional<Date> parseCompactDate(std::string_view aText);

/// Whether a date and time attribute must carry the time.
enum class TimeOfDay {
    Required, ///< Only `yyyyMMdd;HHmmss` is valid.
    Optional, ///< `yyyyMMdd` is valid too and has no timestamp.
};

/// A date and time attribute: the date as written and the UTC timestamp when the time is given.
struct CompactDateTime {
    Date mDate;
    std::optional<SourceTimestamp> mTimestamp;
};

/// Reads `yyyyMMdd;HHmmss` (US Eastern time) or, when aTimeOfDay is Optional, `yyyyMMdd`. Any other
/// text, and a time that does not exist, give no value.
[[nodiscard]] std::optional<CompactDateTime> parseCompactDateTime(std::string_view aText,
                                                                  TimeOfDay aTimeOfDay);

} // namespace taxbroker::ibkr
