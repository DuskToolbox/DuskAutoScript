#include <das/Core/IPC/IpcPermissions.h>
#include <das/Core/Logger/Logger.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <sddl.h>
#include <vector>
#include <windows.h>
#endif

DAS_CORE_IPC_NS_BEGIN

IpcSecurityAttributes::IpcSecurityAttributes()
{
#if defined(_WIN32) || defined(__CYGWIN__)
    // Get current process token
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        DAS_CORE_LOG_WARN(
            "IpcSecurityAttributes: OpenProcessToken failed, using default "
            "permissions");
        perm_ = boost::interprocess::permissions();
        return;
    }

    // Get TokenUser information
    DWORD needed = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &needed);
    std::vector<uint8_t> buffer(needed);
    if (!GetTokenInformation(hToken, TokenUser, buffer.data(), needed, &needed))
    {
        DAS_CORE_LOG_WARN(
            "IpcSecurityAttributes: GetTokenInformation failed, using default "
            "permissions");
        CloseHandle(hToken);
        perm_ = boost::interprocess::permissions();
        return;
    }
    CloseHandle(hToken);

    auto* pUserInfo = reinterpret_cast<TOKEN_USER*>(buffer.data());

    // Convert SID to string for SDDL
    LPWSTR sidString = nullptr;
    if (!ConvertSidToStringSidW(pUserInfo->User.Sid, &sidString))
    {
        DAS_CORE_LOG_WARN(
            "IpcSecurityAttributes: ConvertSidToStringSidW failed, using "
            "default permissions");
        perm_ = boost::interprocess::permissions();
        return;
    }

    // Build SDDL: "D:(A;;GA;;;{current-user-sid})"
    // D: = DACL
    // (A;;GA;;;SID) = Allow, Generic All, to SID
    std::wstring sddl = L"D:(A;;GA;;;" + std::wstring(sidString) + L")";
    LocalFree(sidString);

    // Convert SDDL to SECURITY_DESCRIPTOR
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;

    void* pSecurityDescriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &pSecurityDescriptor,
            nullptr))
    {
        DAS_CORE_LOG_WARN(
            "IpcSecurityAttributes: "
            "ConvertStringSecurityDescriptorToSecurityDescriptorW failed, "
            "using default permissions");
        perm_ = boost::interprocess::permissions();
        return;
    }

    security_descriptor_ = pSecurityDescriptor;
    sa.lpSecurityDescriptor = pSecurityDescriptor;

    // Wrap in boost::permissions
    perm_ = boost::interprocess::permissions(&sa);
#else
    // POSIX: owner read/write only
    perm_ = boost::interprocess::permissions(static_cast<mode_t>(0600));
#endif
}

IpcSecurityAttributes::~IpcSecurityAttributes()
{
#if defined(_WIN32) || defined(__CYGWIN__)
    if (security_descriptor_)
    {
        LocalFree(security_descriptor_);
        security_descriptor_ = nullptr;
    }
#endif
}

boost::interprocess::permissions IpcSecurityAttributes::GetPermissions() const
{
    return perm_;
}

DAS_CORE_IPC_NS_END
