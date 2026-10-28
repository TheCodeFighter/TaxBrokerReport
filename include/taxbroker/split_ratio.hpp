#pragma once

#include "taxbroker/types.hpp"

#include <cstdint>
#include <optional>

namespace taxbroker {

/// A reduced number above this in a ratio is unusual and worth a second look, for example
/// 520000001 / 52000000 from a rounded share count.
inline constexpr std::int64_t kUnusualSplitRatioLimit = 1000;

/// The ratio of the numbers the user entered from the announcement, reduced by their greatest
/// common divisor: 20 new for 10 old is 2 / 1. Empty when a number is not positive or both are
/// equal, because then the position would not change.
[[nodiscard]] std::optional<SplitRatio> makeSplitRatio(std::int64_t aNewShares,
                                                       std::int64_t aOldShares);

/// The ratio from the shares held just before and just after the action, as the broker shows
/// them (8 decimals): 0.52 before and 5.20 after is 10 / 1. Empty as for makeSplitRatio.
[[nodiscard]] std::optional<SplitRatio> splitRatioFromShareCounts(Units aSharesBefore,
                                                                  Units aSharesAfter);

/// The position after the action: aPositionBefore * new / old, exact, rounded once to 8 decimals
/// with halves away from zero. Empty when the result is not a positive Units value.
[[nodiscard]] std::optional<Units> previewSplitUnits(Units aPositionBefore,
                                                     const SplitRatio& aRatio);

/// Tells whether either reduced number is above kUnusualSplitRatioLimit. The GUI and the CLI warn
/// about such a ratio; the backend still accepts it after the preview is confirmed.
[[nodiscard]] bool isUnusualSplitRatio(const SplitRatio& aRatio);

} // namespace taxbroker
