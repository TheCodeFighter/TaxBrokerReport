#include "multipart.hpp"

#include "ascii_text.hpp"

#include <utility>

namespace taxbroker {

namespace {

constexpr std::size_t kMaxBoundaryLength = 70;
constexpr std::string_view kLineEnd = "\r\n";

/// Splits a header value at the `;` that are outside quotes.
std::vector<std::string_view> splitParameters(std::string_view aValue) {
    std::vector<std::string_view> parts;
    bool inQuotes = false;
    std::size_t start = 0;

    for (std::size_t index = 0; index < aValue.size(); ++index)
    {
        if (aValue[index] == '"')
        {
            inQuotes = !inQuotes;
        }
        else if (aValue[index] == ';' && !inQuotes)
        {
            parts.push_back(aValue.substr(start, index - start));
            start = index + 1;
        }
    }

    parts.push_back(aValue.substr(start));

    return parts;
}

/// The value of `name=value` when the parameter has that name (without case), quotes removed. A
/// backslash is kept as it is: a file name may be a Windows path.
std::optional<std::string_view> parameterValue(std::string_view aParameter,
                                               std::string_view aName) {
    aParameter = trimBlanks(aParameter);

    const std::size_t equals = aParameter.find('=');

    if (equals == std::string_view::npos ||
        !equalsIgnoringAsciiCase(trimBlanks(aParameter.substr(0, equals)), aName))
    {
        return std::nullopt;
    }

    std::string_view value = trimBlanks(aParameter.substr(equals + 1));

    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    {
        value = value.substr(1, value.size() - 2);
    }

    return value;
}

/// The boundary of a `multipart/form-data` Content-Type, or nothing when the type or the
/// boundary is not valid.
std::optional<std::string_view> boundaryOf(std::string_view aContentType) {
    const std::vector<std::string_view> parts = splitParameters(aContentType);

    if (!equalsIgnoringAsciiCase(trimBlanks(parts.front()), "multipart/form-data"))
    {
        return std::nullopt;
    }

    for (std::size_t index = 1; index < parts.size(); ++index)
    {
        const auto boundary = parameterValue(parts[index], "boundary");

        if (!boundary.has_value())
        {
            continue;
        }

        if (boundary->empty() || boundary->size() > kMaxBoundaryLength)
        {
            return std::nullopt;
        }

        return boundary;
    }

    return std::nullopt;
}

/// The position after a delimiter line that starts at aPosition (`--boundary`), or nothing when
/// the text there is not a delimiter line. aClosing is true for the closing line `--boundary--`.
std::optional<std::size_t> delimiterEnd(std::string_view aBody,
                                        std::size_t aPosition,
                                        std::size_t aDelimiterLength,
                                        bool& aClosing) {
    std::size_t position = aPosition + aDelimiterLength;

    aClosing = aBody.substr(position, 2) == "--";

    if (aClosing)
    {
        return position + 2;
    }

    // Blanks after the delimiter are allowed; a letter is not: the text is part of the content.
    while (position < aBody.size() && isBlank(aBody[position]))
    {
        ++position;
    }

    if (aBody.substr(position, kLineEnd.size()) != kLineEnd)
    {
        return std::nullopt;
    }

    return position + kLineEnd.size();
}

/// What the header lines of one part say.
struct PartHeaders {
    std::string mName;                    ///< The `name` parameter; empty when there is none.
    std::optional<std::string> mFileName; ///< The raw `filename` parameter; none for a field.
    bool mHasDisposition = false;         ///< True once a `form-data` disposition was read.
};

/// The part header lines up to the blank line, read from aPosition. Moves aPosition behind the
/// blank line. Nothing when a line has no end or a header has no colon.
std::optional<PartHeaders> readPartHeaders(std::string_view aBody, std::size_t& aPosition) {
    PartHeaders headers;

    while (true)
    {
        const std::size_t end = aBody.find(kLineEnd, aPosition);

        if (end == std::string_view::npos)
        {
            return std::nullopt;
        }

        const std::string_view line = aBody.substr(aPosition, end - aPosition);

        aPosition = end + kLineEnd.size();

        if (line.empty())
        {
            return headers;
        }

        const std::size_t colon = line.find(':');

        if (colon == std::string_view::npos)
        {
            return std::nullopt;
        }

        if (!equalsIgnoringAsciiCase(trimBlanks(line.substr(0, colon)), "content-disposition"))
        {
            continue;
        }

        const std::vector<std::string_view> parameters = splitParameters(line.substr(colon + 1));

        if (!equalsIgnoringAsciiCase(trimBlanks(parameters.front()), "form-data"))
        {
            return std::nullopt;
        }

        headers.mHasDisposition = true;

        for (std::size_t index = 1; index < parameters.size(); ++index)
        {
            if (const auto name = parameterValue(parameters[index], "name"))
            {
                headers.mName = std::string{*name};
            }
            else if (const auto fileName = parameterValue(parameters[index], "filename"))
            {
                headers.mFileName = std::string{*fileName};
            }
        }
    }
}

} // namespace

std::string_view multipartErrorReason(MultipartError aError) {
    switch (aError)
    {
    case MultipartError::BadContentType:
        return "content_type";
    case MultipartError::Malformed:
        return "body";
    case MultipartError::BadPartHeader:
        return "part_header";
    case MultipartError::TooManyParts:
        return "too_many_parts";
    case MultipartError::TooLarge:
        return "too_large";
    }

    return "body";
}

std::variant<std::vector<MultipartPart>, MultipartError>
readMultipart(std::string_view aBody, std::string_view aContentType, std::size_t aMaxFileBytes) {
    const auto boundary = boundaryOf(aContentType);

    if (!boundary.has_value())
    {
        return MultipartError::BadContentType;
    }

    const std::string firstDelimiter = "--" + std::string{*boundary};
    const std::string nextDelimiter = std::string{kLineEnd} + firstDelimiter;

    // The first delimiter is at the very start or after a line end; text before it is ignored.
    std::size_t position = 0;
    bool closing = false;
    std::optional<std::size_t> afterDelimiter;

    if (aBody.starts_with(firstDelimiter))
    {
        afterDelimiter = delimiterEnd(aBody, 0, firstDelimiter.size(), closing);
    }

    while (!afterDelimiter.has_value())
    {
        position = aBody.find(nextDelimiter, position);

        if (position == std::string_view::npos)
        {
            return MultipartError::Malformed;
        }

        afterDelimiter =
            delimiterEnd(aBody, position + kLineEnd.size(), firstDelimiter.size(), closing);
        ++position;
    }

    std::vector<MultipartPart> parts;

    while (!closing)
    {
        if (parts.size() == kMaxMultipartParts)
        {
            return MultipartError::TooManyParts;
        }

        std::size_t contentStart = *afterDelimiter;
        auto headers = readPartHeaders(aBody, contentStart);

        if (!headers.has_value() || !headers->mHasDisposition || headers->mName.empty())
        {
            return MultipartError::BadPartHeader;
        }

        // The content ends at the next delimiter line; a delimiter text followed by a letter is
        // content.
        afterDelimiter.reset();
        position = contentStart;
        std::size_t contentEnd = 0;

        while (!afterDelimiter.has_value())
        {
            contentEnd = aBody.find(nextDelimiter, position);

            if (contentEnd == std::string_view::npos)
            {
                return MultipartError::Malformed;
            }

            afterDelimiter =
                delimiterEnd(aBody, contentEnd + kLineEnd.size(), firstDelimiter.size(), closing);
            position = contentEnd + 1;
        }

        const std::string_view content = aBody.substr(contentStart, contentEnd - contentStart);

        if (headers->mFileName.has_value() && content.size() > aMaxFileBytes)
        {
            return MultipartError::TooLarge;
        }

        parts.push_back(MultipartPart{.mName = std::move(headers->mName),
                                      .mFileName = std::move(headers->mFileName),
                                      .mContent = content});
    }

    return parts;
}

} // namespace taxbroker
