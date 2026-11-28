// Windows only: the access lists, owners, links and junctions of the private file and folder
// code. Linux and macOS check the same rules with POSIX modes in the profile store, launch file
// and state folder tests. The file is added to the test program only on Windows.

#include "server/launch_file.hpp"
#include "server/profile_store.hpp"
#include "taxbroker/private_file.hpp"
#include "taxbroker/state_folder.hpp"
#include "utils/windows_security.hpp"

#include <gtest/gtest.h>

#include <aclapi.h>
#include <sddl.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {
using namespace taxbroker;

/// A fresh, empty folder for one test; removed before and after.
class TempFolder {
  public:
    explicit TempFolder(const std::string& aName)
        : mPath(std::filesystem::temp_directory_path() / ("tbr_win_" + aName)) {
        std::filesystem::remove_all(mPath);
        std::filesystem::create_directories(mPath);
    }

    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(mPath, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return mPath;
    }

  private:
    std::filesystem::path mPath; ///< Always set by the constructor.
};

/// What the access list and the owner of a file or folder hold.
struct AccessInfo {
    std::vector<unsigned char> mOwner;                ///< The owner SID.
    std::vector<std::vector<unsigned char>> mAllowed; ///< The SID of each allow entry.
    std::vector<BYTE> mFlags;                         ///< The ACE flags of each allow entry.
    bool mHasInheritedEntry;                          ///< True when an entry came from the parent.
    bool mIsProtected;                                ///< True when the list ignores the parent.
};

std::vector<unsigned char> sidBytes(PSID aSid) {
    const DWORD length = ::GetLengthSid(aSid);
    const unsigned char* begin = reinterpret_cast<const unsigned char*>(aSid);

    return std::vector<unsigned char>(begin, begin + length);
}

/// Reads the owner and the access list by name (not following a link).
AccessInfo accessOf(const std::filesystem::path& aPath) {
    AccessInfo info{};
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;

    const DWORD result =
        ::GetNamedSecurityInfoW(aPath.c_str(),
                                SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                &owner,
                                nullptr,
                                &dacl,
                                nullptr,
                                &descriptor);

    EXPECT_EQ(result, static_cast<DWORD>(ERROR_SUCCESS));

    if (result != ERROR_SUCCESS || dacl == nullptr)
    {
        return info;
    }

    info.mOwner = sidBytes(owner);

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    ::GetSecurityDescriptorControl(descriptor, &control, &revision);
    info.mIsProtected = (control & SE_DACL_PROTECTED) != 0;

    ACL_SIZE_INFORMATION size{};
    ::GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation);

    for (DWORD index = 0; index < size.AceCount; ++index)
    {
        void* entry = nullptr;
        ::GetAce(dacl, index, &entry);

        const ACE_HEADER* header = reinterpret_cast<const ACE_HEADER*>(entry);

        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
        {
            continue;
        }

        const ACCESS_ALLOWED_ACE* allowed = reinterpret_cast<const ACCESS_ALLOWED_ACE*>(entry);

        info.mAllowed.push_back(
            sidBytes(const_cast<PSID>(reinterpret_cast<const void*>(&allowed->SidStart))));
        info.mFlags.push_back(header->AceFlags);

        if ((header->AceFlags & INHERITED_ACE) != 0)
        {
            info.mHasInheritedEntry = true;
        }
    }

    ::LocalFree(descriptor);

    return info;
}

/// True when the SID bytes are the SID of the current user.
bool isCurrentUser(const std::vector<unsigned char>& aSid) {
    const std::vector<unsigned char> user = windows::currentUserSid();

    return windows::sameSid(aSid.data(), user.data());
}

/// A file or folder with an access list that lets Everyone in (S-1-1-0, "WD").
SECURITY_ATTRIBUTES everyoneAttributes(PSECURITY_DESCRIPTOR* aDescriptor) {
    EXPECT_TRUE(::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;WD)",
                                                                       SDDL_REVISION_1,
                                                                       aDescriptor,
                                                                       nullptr));

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = *aDescriptor;
    attributes.bInheritHandle = FALSE;

    return attributes;
}

/// Makes a junction with `mklink /J`; a junction needs no special right. False when it failed.
bool makeJunction(const std::filesystem::path& aLink, const std::filesystem::path& aTarget) {
    const std::string command =
        "mklink /J \"" + aLink.string() + "\" \"" + aTarget.string() + "\" > nul";

    return std::system(command.c_str()) == 0 && std::filesystem::exists(aLink);
}

/// The text of the exception that the call throws, or "no exception".
std::string messageOfThrow(const std::function<void()>& aCall) {
    try
    { aCall(); } catch (const std::runtime_error& error)
    { return error.what(); }

    return "no exception";
}

std::string readAll(const std::filesystem::path& aPath) {
    std::ifstream file(aPath, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();

    return content.str();
}

/// Switches a privilege on for this process. False when the user does not have it.
bool enablePrivilege(const wchar_t* aName) {
    HANDLE token = nullptr;

    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    {
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    bool enabled = ::LookupPrivilegeValueW(nullptr, aName, &privileges.Privileges[0].Luid) &&
                   ::AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) &&
                   ::GetLastError() == ERROR_SUCCESS;

    ::CloseHandle(token);

    return enabled;
}

/// Gives the folder another owner. False when the process may not do that.
bool setOwner(const std::filesystem::path& aFolder, std::vector<unsigned char>& aSid) {
    if (!enablePrivilege(L"SeRestorePrivilege") || !enablePrivilege(L"SeTakeOwnershipPrivilege"))
    {
        return false;
    }

    return ::SetNamedSecurityInfoW(const_cast<LPWSTR>(aFolder.c_str()),
                                   SE_FILE_OBJECT,
                                   OWNER_SECURITY_INFORMATION,
                                   aSid.data(),
                                   nullptr,
                                   nullptr,
                                   nullptr) == ERROR_SUCCESS;
}

// ----- The private file -----

TEST(WindowsPrivateFileTest, AWrittenFileHasOneAccessEntryForTheCurrentUser) {
    const TempFolder temp("file_access");
    const std::filesystem::path file = temp.path() / "profile.json";

    writePrivateFile(file, "content");

    const AccessInfo info = accessOf(file);

    EXPECT_EQ(readAll(file), "content");
    EXPECT_TRUE(info.mIsProtected);
    EXPECT_FALSE(info.mHasInheritedEntry);
    EXPECT_TRUE(isCurrentUser(info.mOwner));

    ASSERT_EQ(info.mAllowed.size(), 1U);
    EXPECT_TRUE(isCurrentUser(info.mAllowed[0]));
}

TEST(WindowsPrivateFileTest, AStaleTemporaryFileWithAnOpenAccessListIsReplaced) {
    const TempFolder temp("stale_temporary");
    const std::filesystem::path file = temp.path() / "profile.json";
    const std::filesystem::path temporary = temp.path() / "profile.json.tmp";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes = everyoneAttributes(&descriptor);

    // The stale file lets Everyone in; CREATE_ALWAYS would have kept that list.
    {
        const windows::Handle stale(::CreateFileW(temporary.c_str(),
                                                  GENERIC_WRITE,
                                                  0,
                                                  &attributes,
                                                  CREATE_NEW,
                                                  FILE_ATTRIBUTE_NORMAL,
                                                  nullptr));

        ASSERT_TRUE(stale.valid());
    }

    ::LocalFree(descriptor);

    writePrivateFile(file, "content");

    const AccessInfo info = accessOf(file);

    EXPECT_EQ(readAll(file), "content");
    EXPECT_FALSE(std::filesystem::exists(temporary));

    ASSERT_EQ(info.mAllowed.size(), 1U);
    EXPECT_TRUE(isCurrentUser(info.mAllowed[0]));
}

TEST(WindowsPrivateFileTest, ALinkAtTheFileNameIsRefusedAndItsTargetIsKept) {
    const TempFolder temp("link_at_name");
    const std::filesystem::path target = temp.path() / "target.txt";
    const std::filesystem::path link = temp.path() / "profile.json";
    const ProfileStore store(link, std::nullopt);

    std::ofstream(target) << "target content";

    std::error_code error;
    std::filesystem::create_symlink(target, link, error);

    if (error)
    {
        GTEST_SKIP() << "the runner may not make symbolic links (needs the right to make links)";
    }

    EXPECT_EQ(messageOfThrow([&]() { store.write("new content"); }),
              "the profile file is a symbolic link");
    EXPECT_EQ(readAll(target), "target content");
    EXPECT_TRUE(std::filesystem::is_symlink(link));
}

TEST(WindowsPrivateFileTest, ALinkAtTheTemporaryNameIsRefusedAndKept) {
    const TempFolder temp("link_at_temporary");
    const std::filesystem::path target = temp.path() / "target.txt";
    const std::filesystem::path link = temp.path() / "profile.json.tmp";
    const ProfileStore store(temp.path() / "profile.json", std::nullopt);

    std::ofstream(target) << "target content";

    std::error_code error;
    std::filesystem::create_symlink(target, link, error);

    if (error)
    {
        GTEST_SKIP() << "the runner may not make symbolic links (needs the right to make links)";
    }

    // The same as O_NOFOLLOW on Linux: the link is refused, not deleted, and not written through.
    EXPECT_EQ(messageOfThrow([&]() { store.write("new content"); }),
              "cannot write the profile file");
    EXPECT_EQ(readAll(target), "target content");
    EXPECT_TRUE(std::filesystem::is_symlink(link));
}

TEST(WindowsPrivateFileTest, AJunctionAtTheFileNameIsRefusedAndItsTargetIsKept) {
    const TempFolder temp("junction_at_name");
    const std::filesystem::path target = temp.path() / "target";
    const std::filesystem::path junction = temp.path() / "profile.json";
    const ProfileStore store(junction, std::nullopt);

    std::filesystem::create_directories(target);
    std::ofstream(target / "inside.txt") << "inside";

    // A junction needs no special right, so this test never skips.
    ASSERT_TRUE(makeJunction(junction, target));

    EXPECT_EQ(messageOfThrow([&]() { store.write("new content"); }),
              "the profile file is a symbolic link");
    EXPECT_EQ(readAll(target / "inside.txt"), "inside");
}

TEST(WindowsPrivateFileTest, AJunctionAtTheTemporaryNameIsRefusedAndKept) {
    const TempFolder temp("junction_at_temporary");
    const std::filesystem::path target = temp.path() / "target";
    const std::filesystem::path junction = temp.path() / "profile.json.tmp";
    const ProfileStore store(temp.path() / "profile.json", std::nullopt);

    std::filesystem::create_directories(target);
    std::ofstream(target / "inside.txt") << "inside";

    ASSERT_TRUE(makeJunction(junction, target));

    EXPECT_EQ(messageOfThrow([&]() { store.write("new content"); }),
              "cannot write the profile file");
    EXPECT_EQ(readAll(target / "inside.txt"), "inside");
    EXPECT_TRUE(std::filesystem::exists(junction));
}

TEST(WindowsPrivateFileTest, AJunctionAtTheFileNameIsRefusedOnRead) {
    const TempFolder temp("junction_read");
    const std::filesystem::path target = temp.path() / "target";
    const std::filesystem::path junction = temp.path() / "profile.json";
    const ProfileStore store(junction, std::nullopt);

    std::filesystem::create_directories(target);

    ASSERT_TRUE(makeJunction(junction, target));

    EXPECT_EQ(messageOfThrow([&]() { (void)store.read(100); }),
              "the profile file is not a regular file");
}

TEST(WindowsPrivateFileTest, ASymbolicLinkAtTheFileNameIsRefusedOnRead) {
    const TempFolder temp("link_read");
    const std::filesystem::path target = temp.path() / "target.txt";
    const std::filesystem::path link = temp.path() / "profile.json";
    const ProfileStore store(link, std::nullopt);

    std::ofstream(target) << "target content";

    std::error_code error;
    std::filesystem::create_symlink(target, link, error);

    if (error)
    {
        GTEST_SKIP() << "the runner may not make symbolic links (needs the right to make links)";
    }

    // The link itself is opened, not its target, and the handle check refuses it. Linux refuses
    // it one step earlier (O_NOFOLLOW) with "cannot open the profile file".
    EXPECT_EQ(messageOfThrow([&]() { (void)store.read(100); }),
              "the profile file is not a regular file");
}

TEST(WindowsPrivateFileTest, ANamedPipeIsNotReadAsAFile) {
    // A path inside a folder is never a pipe, so ProfileStore cannot be given one; the read
    // function is. FIRST_PIPE_INSTANCE makes the test fail when another program owns the name.
    const std::wstring name = L"\\\\.\\pipe\\tbr_test_" + std::to_wstring(::GetCurrentProcessId());
    const windows::Handle server(
        ::CreateNamedPipeW(name.c_str(),
                           PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                           PIPE_TYPE_BYTE,
                           1,
                           512,
                           512,
                           0,
                           nullptr));

    ASSERT_TRUE(server.valid());

    try
    {
        (void)readPrivateFile(std::filesystem::path(name), 100);
        FAIL() << "expected PrivateFileError";
    } catch (const PrivateFileError& error)
    {
        EXPECT_EQ(error.problem(), PrivateFileProblem::NotRegular);
        EXPECT_STREQ(error.what(), "the file is not a regular file");
    }
}

TEST(WindowsPrivateFileTest, AFileThatIsNotThereIsReadAsNothing) {
    const TempFolder temp("read_missing");

    EXPECT_EQ(readPrivateFile(temp.path() / "missing.json", 100), std::nullopt);
}

TEST(WindowsPrivateFileTest, ALaunchFileHasOneAccessEntryAndIsReplaced) {
    const TempFolder temp("launch_file");
    const std::filesystem::path file = temp.path() / "launch.html";

    writeLaunchFile(file, "first");
    writeLaunchFile(file, "second");

    const AccessInfo info = accessOf(file);

    EXPECT_EQ(readAll(file), "second");

    ASSERT_EQ(info.mAllowed.size(), 1U);
    EXPECT_TRUE(isCurrentUser(info.mAllowed[0]));
    EXPECT_TRUE(isCurrentUser(info.mOwner));
}

TEST(WindowsPrivateFileTest, ALaunchFileIsNotWrittenThroughAJunction) {
    const TempFolder temp("launch_junction");
    const std::filesystem::path target = temp.path() / "target";
    const std::filesystem::path junction = temp.path() / "launch.html";

    std::filesystem::create_directories(target);

    ASSERT_TRUE(makeJunction(junction, target));

    EXPECT_EQ(messageOfThrow([&]() { writeLaunchFile(junction, "page"); }),
              "cannot write the launch file");
}

TEST(WindowsPrivateFileTest, ARemovedFileIsGoneAndAMissingFileIsFine) {
    const TempFolder temp("remove");
    const std::filesystem::path file = temp.path() / "profile.json";

    writePrivateFile(file, "content");
    removePrivateFile(file);

    EXPECT_FALSE(std::filesystem::exists(file));

    removePrivateFile(file);
}

// ----- The private folder -----

TEST(WindowsPrivateFolderTest, ANewFolderHasOneInheritableEntryForTheCurrentUser) {
    const TempFolder temp("new_folder");
    const std::filesystem::path folder = temp.path() / "a" / "taxbroker";

    makePrivateFolder(folder);

    const AccessInfo info = accessOf(folder);

    EXPECT_TRUE(std::filesystem::is_directory(folder));
    EXPECT_TRUE(info.mIsProtected);
    EXPECT_TRUE(isCurrentUser(info.mOwner));

    ASSERT_EQ(info.mAllowed.size(), 1U);
    EXPECT_TRUE(isCurrentUser(info.mAllowed[0]));
    EXPECT_EQ(info.mFlags[0] & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE),
              OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
}

TEST(WindowsPrivateFolderTest, AnExistingFolderWithAnOpenAccessListGetsTheSameList) {
    const TempFolder temp("open_folder");
    const std::filesystem::path folder = temp.path() / "taxbroker";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes = everyoneAttributes(&descriptor);

    ASSERT_TRUE(::CreateDirectoryW(folder.c_str(), &attributes));

    ::LocalFree(descriptor);

    makePrivateFolder(folder);

    const AccessInfo info = accessOf(folder);

    EXPECT_TRUE(info.mIsProtected);

    ASSERT_EQ(info.mAllowed.size(), 1U);
    EXPECT_TRUE(isCurrentUser(info.mAllowed[0]));
}

TEST(WindowsPrivateFolderTest, AFileMadeInTheFolderInheritsTheListOfTheUser) {
    const TempFolder temp("inherit");
    const std::filesystem::path folder = temp.path() / "taxbroker";

    makePrivateFolder(folder);

    // The logger makes its file like this: a plain open, with no access list of its own.
    std::ofstream(folder / "taxbroker.log") << "line";

    const AccessInfo info = accessOf(folder / "taxbroker.log");

    EXPECT_TRUE(info.mHasInheritedEntry);

    ASSERT_EQ(info.mAllowed.size(), 1U);
    EXPECT_TRUE(isCurrentUser(info.mAllowed[0]));
}

TEST(WindowsPrivateFolderTest, AJunctionAsTheFolderIsRefused) {
    const TempFolder temp("junction_folder");
    const std::filesystem::path target = temp.path() / "target";
    const std::filesystem::path junction = temp.path() / "link";

    std::filesystem::create_directories(target);

    ASSERT_TRUE(makeJunction(junction, target));

    EXPECT_EQ(messageOfThrow([&]() { makePrivateFolder(junction); }),
              "the private folder is a symbolic link");
}

TEST(WindowsPrivateFolderTest, AFolderBelowAFileIsRefused) {
    const TempFolder temp("below_file");
    const std::filesystem::path file = temp.path() / "taxbroker";

    std::ofstream(file) << "not a folder";

    // The parent folders are made first, and that fails on the file (Linux fails one step later,
    // at the check of the folder, with "cannot check the private folder").
    EXPECT_EQ(messageOfThrow([&]() { makePrivateFolder(file / "inner"); }),
              "cannot create the private folder");
}

TEST(WindowsPrivateFolderTest, AFolderOwnedByTheAdministratorsGroupIsAcceptedAndGetsTheList) {
    const TempFolder temp("owner_administrators");
    const std::filesystem::path folder = temp.path() / "taxbroker";
    std::vector<unsigned char> administrators = windows::wellKnownSid(WinBuiltinAdministratorsSid);

    std::filesystem::create_directories(folder);

    if (!setOwner(folder, administrators))
    {
        GTEST_SKIP() << "the process may not give a folder another owner (needs a privilege)";
    }

    makePrivateFolder(folder);

    const AccessInfo info = accessOf(folder);

    ASSERT_EQ(info.mAllowed.size(), 1U);
    EXPECT_TRUE(isCurrentUser(info.mAllowed[0]));
}

TEST(WindowsPrivateFolderTest, AFolderOwnedByAnotherAccountIsRefusedWithoutThePath) {
    const TempFolder temp("owner_other");
    const std::filesystem::path folder = temp.path() / "taxbroker";
    std::vector<unsigned char> localService = windows::wellKnownSid(WinLocalServiceSid);

    std::filesystem::create_directories(folder);

    if (!setOwner(folder, localService))
    {
        GTEST_SKIP() << "the process may not give a folder another owner (needs a privilege)";
    }

    EXPECT_EQ(messageOfThrow([&]() { makePrivateFolder(folder); }),
              "the private folder belongs to another user");
}

} // namespace
