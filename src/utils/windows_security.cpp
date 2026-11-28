#include "windows_security.hpp"

#if defined(_WIN32)

#include <sddl.h>

#include <stdexcept>

namespace taxbroker::windows {

namespace {

/// The SID as text such as `S-1-5-21-...`, for the descriptor string.
std::wstring sidText(const void* aSid) {
    wchar_t* text = nullptr;

    if (!::ConvertSidToStringSidW(const_cast<PSID>(aSid), &text))
    {
        throw std::runtime_error("cannot read the user of the process");
    }

    std::wstring result(text);
    ::LocalFree(text);

    return result;
}

} // namespace

std::vector<unsigned char> currentUserSid() {
    HANDLE token = nullptr;

    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        throw std::runtime_error("cannot read the user of the process");
    }

    DWORD size = 0;

    // The first call only gives the size.
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);

    std::vector<unsigned char> buffer(size);

    if (size == 0 || !::GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
    {
        ::CloseHandle(token);

        throw std::runtime_error("cannot read the user of the process");
    }

    ::CloseHandle(token);

    const TOKEN_USER* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    const DWORD length = ::GetLengthSid(user->User.Sid);
    const unsigned char* begin = reinterpret_cast<const unsigned char*>(user->User.Sid);

    return std::vector<unsigned char>(begin, begin + length);
}

std::vector<unsigned char> wellKnownSid(WELL_KNOWN_SID_TYPE aType) {
    DWORD size = SECURITY_MAX_SID_SIZE;
    std::vector<unsigned char> sid(size);

    if (!::CreateWellKnownSid(aType, nullptr, sid.data(), &size))
    {
        throw std::runtime_error("cannot read a system account");
    }

    sid.resize(size);

    return sid;
}

bool sameSid(const void* aFirst, const void* aSecond) {
    return aFirst != nullptr && aSecond != nullptr && ::IsValidSid(const_cast<PSID>(aFirst)) &&
           ::IsValidSid(const_cast<PSID>(aSecond)) &&
           ::EqualSid(const_cast<PSID>(aFirst), const_cast<PSID>(aSecond));
}

UserOnlyDescriptor::UserOnlyDescriptor(bool aInheritable) {
    const std::vector<unsigned char> user = currentUserSid();
    const std::wstring sid = sidText(user.data());

    // O: owner. D:P: a protected list. One allow entry with all file rights for the user; OICI
    // passes it on to the files and folders made inside the folder.
    const std::wstring text =
        L"O:" + sid + L"D:P(A;" + (aInheritable ? L"OICI" : L"") + L";FA;;;" + sid + L")";

    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(),
                                                                SDDL_REVISION_1,
                                                                &mDescriptor,
                                                                nullptr))
    {
        throw std::runtime_error("cannot make the access list");
    }
}

UserOnlyDescriptor::~UserOnlyDescriptor() {
    ::LocalFree(mDescriptor);
}

SECURITY_ATTRIBUTES UserOnlyDescriptor::attributes() {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = mDescriptor;
    attributes.bInheritHandle = FALSE;

    return attributes;
}

PACL UserOnlyDescriptor::dacl() const {
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL list = nullptr;

    if (!::GetSecurityDescriptorDacl(mDescriptor, &present, &list, &defaulted) || !present)
    {
        throw std::runtime_error("cannot make the access list");
    }

    return list;
}

std::optional<DWORD> attributesOf(const std::filesystem::path& aPath) {
    const DWORD attributes = ::GetFileAttributesW(aPath.c_str());

    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        return attributes;
    }

    const DWORD error = ::GetLastError();

    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
    {
        return std::nullopt;
    }

    throw std::runtime_error("cannot check a file");
}

} // namespace taxbroker::windows

#endif
