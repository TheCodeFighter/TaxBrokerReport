#include "test_environment.hpp"

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <stdlib.h> // _wputenv_s
#endif

namespace taxbroker::test {

#if defined(_WIN32)

namespace {

/// UTF-8 text as wide characters. The tests only pass valid text.
std::wstring widen(std::string_view aText) {
    if (aText.empty())
    {
        return {};
    }

    const int count = ::MultiByteToWideChar(CP_UTF8,
                                            MB_ERR_INVALID_CHARS,
                                            aText.data(),
                                            static_cast<int>(aText.size()),
                                            nullptr,
                                            0);
    std::wstring wide(static_cast<std::size_t>(count), L'\0');

    ::MultiByteToWideChar(CP_UTF8,
                          MB_ERR_INVALID_CHARS,
                          aText.data(),
                          static_cast<int>(aText.size()),
                          wide.data(),
                          count);

    return wide;
}

} // namespace

void setTestEnvironment(std::string_view aName, std::string_view aValue) {
    ::_wputenv_s(widen(aName).c_str(), widen(aValue).c_str());
}

void unsetTestEnvironment(std::string_view aName) {
    // An empty value removes the variable.
    ::_wputenv_s(widen(aName).c_str(), L"");
}

#else

void setTestEnvironment(std::string_view aName, std::string_view aValue) {
    ::setenv(std::string(aName).c_str(), std::string(aValue).c_str(), 1);
}

void unsetTestEnvironment(std::string_view aName) {
    ::unsetenv(std::string(aName).c_str());
}

#endif

} // namespace taxbroker::test
