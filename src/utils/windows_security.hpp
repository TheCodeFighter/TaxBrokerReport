#pragma once

// Windows only: the user's SID and a user-only security descriptor, shared by the private folder
// (state_folder.cpp) and the private file (private_file.cpp). Nothing here names a path.

#if defined(_WIN32)

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace taxbroker::windows {

/// The SID of the user of this process (TokenUser, not TokenOwner: in an elevated process the
/// default owner is the Administrators group). Throws `std::runtime_error` when it cannot be read.
[[nodiscard]] std::vector<unsigned char> currentUserSid();

/// A well-known SID, for example `WinBuiltinAdministratorsSid` or `WinLocalSystemSid`.
[[nodiscard]] std::vector<unsigned char> wellKnownSid(WELL_KNOWN_SID_TYPE aType);

/// True when both SIDs are valid and equal.
[[nodiscard]] bool sameSid(const void* aFirst, const void* aSecond);

/// Closes a handle in the destructor. `CreateFileW` gives `INVALID_HANDLE_VALUE` for a failure.
class Handle {
  public:
    explicit Handle(HANDLE aHandle) : mHandle(aHandle) {}

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    ~Handle() {
        if (mHandle != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(mHandle);
        }
    }

    [[nodiscard]] HANDLE get() const {
        return mHandle;
    }

    [[nodiscard]] bool valid() const {
        return mHandle != INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE mHandle; ///< Always set by the constructor.
};

/// A security descriptor with the user as owner and one access entry for the user (full access)
/// on a protected list (no entry is inherited). An inheritable list passes the entry on to files
/// and folders made inside a folder.
class UserOnlyDescriptor {
  public:
    explicit UserOnlyDescriptor(bool aInheritable);

    UserOnlyDescriptor(const UserOnlyDescriptor&) = delete;
    UserOnlyDescriptor& operator=(const UserOnlyDescriptor&) = delete;

    ~UserOnlyDescriptor();

    /// For `CreateFileW` and `CreateDirectoryW`. Valid while this object lives.
    [[nodiscard]] SECURITY_ATTRIBUTES attributes();

    /// The access list, for `SetSecurityInfo`. Valid while this object lives.
    [[nodiscard]] PACL dacl() const;

  private:
    PSECURITY_DESCRIPTOR mDescriptor =
        nullptr; ///< Allocated by the system, freed in the destructor.
};

/// The attributes of a path itself (a link is not followed), or nothing when it does not exist.
/// Throws `std::runtime_error` for any other failure.
[[nodiscard]] std::optional<DWORD> attributesOf(const std::filesystem::path& aPath);

} // namespace taxbroker::windows

#endif
