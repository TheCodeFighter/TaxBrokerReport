#include "taxbroker/environment.hpp"

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <vector>
#endif

namespace taxbroker {

#if defined(_WIN32)

std::optional<std::string> readEnvironment(std::string_view aName) {
    // A variable name is plain ASCII; anything else is not a name this program asks for.
    std::wstring name;
    name.reserve(aName.size());

    for (const char character : aName)
    {
        if (character <= 0 || character >= 0x7f)
        {
            return std::nullopt;
        }

        name.push_back(static_cast<wchar_t>(character));
    }

    // With no buffer the call returns the size needed, including the end character, or 0 when the
    // variable is not set.
    const DWORD needed = ::GetEnvironmentVariableW(name.c_str(), nullptr, 0);

    if (needed == 0)
    {
        return std::nullopt;
    }

    std::vector<wchar_t> wide(needed);
    const DWORD length = ::GetEnvironmentVariableW(name.c_str(), wide.data(), needed);

    // A value that changed between the two calls is treated as not readable.
    if (length == 0 || length >= needed)
    {
        return std::nullopt;
    }

    const int bytes = ::WideCharToMultiByte(CP_UTF8,
                                            WC_ERR_INVALID_CHARS,
                                            wide.data(),
                                            static_cast<int>(length),
                                            nullptr,
                                            0,
                                            nullptr,
                                            nullptr);

    if (bytes <= 0)
    {
        return std::nullopt;
    }

    std::string text(static_cast<std::size_t>(bytes), '\0');

    ::WideCharToMultiByte(CP_UTF8,
                          WC_ERR_INVALID_CHARS,
                          wide.data(),
                          static_cast<int>(length),
                          text.data(),
                          bytes,
                          nullptr,
                          nullptr);

    return text;
}

std::filesystem::path pathFromUtf8(std::string_view aText) {
    if (aText.empty())
    {
        return {};
    }

    const int characters = ::MultiByteToWideChar(CP_UTF8,
                                                 MB_ERR_INVALID_CHARS,
                                                 aText.data(),
                                                 static_cast<int>(aText.size()),
                                                 nullptr,
                                                 0);

    // Bytes that are not UTF-8 cannot be a valid wide path.
    if (characters <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(characters), L'\0');

    ::MultiByteToWideChar(CP_UTF8,
                          MB_ERR_INVALID_CHARS,
                          aText.data(),
                          static_cast<int>(aText.size()),
                          wide.data(),
                          characters);

    return std::filesystem::path(wide);
}

#else

std::optional<std::string> readEnvironment(std::string_view aName) {
    // std::getenv needs a terminated name, and the value is copied at once.
    const char* value = std::getenv(std::string(aName).c_str());

    if (value == nullptr)
    {
        return std::nullopt;
    }

    return std::string(value);
}

std::filesystem::path pathFromUtf8(std::string_view aText) {
    const std::u8string text(reinterpret_cast<const char8_t*>(aText.data()), aText.size());

    return std::filesystem::path(text);
}

#endif

} // namespace taxbroker
