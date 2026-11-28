#include "taxbroker/state_folder.hpp"

#include "taxbroker/environment.hpp"

#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#include "windows_security.hpp"

#include <aclapi.h>
#endif

namespace taxbroker {

EnvLookup systemEnvironment() {
    return
        [](std::string_view aName) -> std::optional<std::string> { return readEnvironment(aName); };
}

std::optional<std::filesystem::path> absoluteEnvironmentPath(const EnvLookup& aEnv,
                                                             std::string_view aName) {
    const std::optional<std::string> value = aEnv(aName);

    if (!value.has_value())
    {
        return std::nullopt;
    }

    const std::filesystem::path path = pathFromUtf8(*value);

    if (!path.is_absolute())
    {
        return std::nullopt;
    }

    return path;
}

std::filesystem::path stateFolder(const EnvLookup& aEnv) {
    // The XDG rule: a relative XDG_STATE_HOME is ignored.
    const auto stateHome = absoluteEnvironmentPath(aEnv, "XDG_STATE_HOME");

    if (stateHome.has_value())
    {
        return *stateHome / "taxbroker";
    }

    const auto home = absoluteEnvironmentPath(aEnv, "HOME");

    if (!home.has_value())
    {
        throw std::runtime_error("cannot find the state folder: HOME is not an absolute path");
    }

    return *home / ".local" / "state" / "taxbroker";
}

#if defined(_WIN32)

namespace {

/// The checks and the access list of the folder, all on one opened handle of the folder itself.
void secureFolderHandle(HANDLE aFolder, windows::UserOnlyDescriptor& aDescriptor) {
    FILE_ATTRIBUTE_TAG_INFO tag{};

    if (!::GetFileInformationByHandleEx(aFolder, FileAttributeTagInfo, &tag, sizeof(tag)))
    {
        throw std::runtime_error("cannot check the private folder");
    }

    // A link or a junction would send the profile to another folder; a file is no folder.
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        throw std::runtime_error("the private folder is a symbolic link");
    }

    if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        throw std::runtime_error("cannot create the private folder");
    }

    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;

    if (::GetSecurityInfo(aFolder,
                          SE_FILE_OBJECT,
                          OWNER_SECURITY_INFORMATION,
                          &owner,
                          nullptr,
                          nullptr,
                          nullptr,
                          &descriptor) != ERROR_SUCCESS)
    {
        throw std::runtime_error("cannot check the private folder");
    }

    // The owner is the user, or an account that can take any file anyway: the Administrators
    // group (the default owner of a folder made by an elevated start) and SYSTEM.
    const std::vector<unsigned char> user = windows::currentUserSid();
    const std::vector<unsigned char> administrators =
        windows::wellKnownSid(WinBuiltinAdministratorsSid);
    const std::vector<unsigned char> localSystem = windows::wellKnownSid(WinLocalSystemSid);
    const bool accepted = windows::sameSid(owner, user.data()) ||
                          windows::sameSid(owner, administrators.data()) ||
                          windows::sameSid(owner, localSystem.data());

    ::LocalFree(descriptor);

    if (!accepted)
    {
        throw std::runtime_error("the private folder belongs to another user");
    }

    // A protected list with one entry, for the user. Files made in the folder inherit it.
    if (::SetSecurityInfo(aFolder,
                          SE_FILE_OBJECT,
                          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                          nullptr,
                          nullptr,
                          aDescriptor.dacl(),
                          nullptr) != ERROR_SUCCESS)
    {
        throw std::runtime_error("cannot set the access list of the private folder");
    }
}

} // namespace

void makePrivateFolder(const std::filesystem::path& aFolder) {
    // The messages never name the folder: it holds the user's name.
    std::error_code error;

    if (aFolder.has_parent_path())
    {
        std::filesystem::create_directories(aFolder.parent_path(), error);

        if (error)
        {
            throw std::runtime_error("cannot create the private folder");
        }
    }

    windows::UserOnlyDescriptor descriptor(true);
    SECURITY_ATTRIBUTES attributes = descriptor.attributes();

    // A new folder gets the list at once. An existing one (also a file or a link at this name)
    // is checked and changed below.
    if (!::CreateDirectoryW(aFolder.c_str(), &attributes) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS)
    {
        throw std::runtime_error("cannot create the private folder");
    }

    const windows::Handle folder(
        ::CreateFileW(aFolder.c_str(),
                      READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr,
                      OPEN_EXISTING,
                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
                          SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS,
                      nullptr));

    if (!folder.valid())
    {
        throw std::runtime_error("cannot check the private folder");
    }

    secureFolderHandle(folder.get(), descriptor);
}

#else

void makePrivateFolder(const std::filesystem::path& aFolder) {
    std::error_code error;

    const bool isLink =
        std::filesystem::is_symlink(std::filesystem::symlink_status(aFolder, error));

    // The messages never name the folder: it holds the user's name.
    if (error && error != std::errc::no_such_file_or_directory)
    {
        throw std::runtime_error("cannot check the private folder");
    }

    if (isLink)
    {
        throw std::runtime_error("the private folder is a symbolic link");
    }

    std::filesystem::create_directories(aFolder, error);

    if (error)
    {
        throw std::runtime_error("cannot create the private folder");
    }

    std::filesystem::permissions(aFolder,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,
                                 error);

    if (error)
    {
        throw std::runtime_error("cannot set the mode of the private folder");
    }
}

#endif

} // namespace taxbroker
