#pragma once

#include <algorithm>
#include <string_view>

namespace taxbroker {

/// ASCII lower case of one character; other bytes are unchanged (no locale).
[[nodiscard]] constexpr char toLowerAscii(char aCharacter) {
    if (aCharacter >= 'A' && aCharacter <= 'Z')
    {
        return static_cast<char>(aCharacter - 'A' + 'a');
    }

    return aCharacter;
}

/// True when the two texts are equal when ASCII letters are compared without case, as for header
/// names, header parameters and the Host and Origin values.
[[nodiscard]] constexpr bool equalsIgnoringAsciiCase(std::string_view aFirst,
                                                     std::string_view aSecond) {
    return std::equal(
        aFirst.begin(),
        aFirst.end(),
        aSecond.begin(),
        aSecond.end(),
        [](char aLeft, char aRight) { return toLowerAscii(aLeft) == toLowerAscii(aRight); });
}

/// True for a space or a tab, the blanks HTTP allows around header values and parameters.
[[nodiscard]] constexpr bool isBlank(char aCharacter) {
    return aCharacter == ' ' || aCharacter == '\t';
}

/// Removes spaces and tabs at both ends.
[[nodiscard]] constexpr std::string_view trimBlanks(std::string_view aText) {
    while (!aText.empty() && isBlank(aText.front()))
    {
        aText.remove_prefix(1);
    }

    while (!aText.empty() && isBlank(aText.back()))
    {
        aText.remove_suffix(1);
    }

    return aText;
}

} // namespace taxbroker
