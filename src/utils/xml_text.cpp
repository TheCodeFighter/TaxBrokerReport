#include "utils/xml_text.hpp"

#include <cstdint>

namespace taxbroker {
namespace {

// Limits of the characters that XML accepts.
constexpr std::uint32_t kFirstPrintableCodePoint = 0x20U; // below it are the control characters
constexpr std::uint32_t kFirstSurrogate = 0xD800U;
constexpr std::uint32_t kLastSurrogate = 0xDFFFU;
constexpr std::uint32_t kNonCharacterFffe = 0xFFFEU;
constexpr std::uint32_t kNonCharacterFfff = 0xFFFFU;
constexpr std::uint32_t kLastCodePoint = 0x10FFFFU;

} // namespace

std::optional<std::size_t> xmlTextLength(std::string_view aText) {
    std::size_t length{};
    std::size_t position{};

    while (position < aText.size())
    {
        const auto lead = static_cast<unsigned char>(aText[position]);
        std::size_t byteCount{};
        std::uint32_t codePoint{};
        std::uint32_t smallestCodePoint{};

        if (lead < 0x80U)
        {
            byteCount = 1;
            codePoint = lead;
        }
        else if ((lead & 0xE0U) == 0xC0U)
        {
            byteCount = 2;
            codePoint = lead & 0x1FU;
            smallestCodePoint = 0x80U;
        }
        else if ((lead & 0xF0U) == 0xE0U)
        {
            byteCount = 3;
            codePoint = lead & 0x0FU;
            smallestCodePoint = 0x800U;
        }
        else if ((lead & 0xF8U) == 0xF0U)
        {
            byteCount = 4;
            codePoint = lead & 0x07U;
            smallestCodePoint = 0x10000U;
        }
        else
        {
            return std::nullopt;
        }

        if (aText.size() - position < byteCount)
        {
            return std::nullopt;
        }

        for (std::size_t next = 1; next < byteCount; ++next)
        {
            const auto continuation = static_cast<unsigned char>(aText[position + next]);

            if ((continuation & 0xC0U) != 0x80U)
            {
                return std::nullopt;
            }

            codePoint = (codePoint << 6U) | (continuation & 0x3FU);
        }

        // A code point written with more bytes than it needs, a UTF-16 surrogate and a value
        // above U+10FFFF are not valid UTF-8.
        if (codePoint < smallestCodePoint ||
            (codePoint >= kFirstSurrogate && codePoint <= kLastSurrogate) ||
            codePoint > kLastCodePoint)
        {
            return std::nullopt;
        }

        if (codePoint < kFirstPrintableCodePoint || codePoint == kNonCharacterFffe ||
            codePoint == kNonCharacterFfff)
        {
            return std::nullopt;
        }

        ++length;
        position += byteCount;
    }

    return length;
}

} // namespace taxbroker
