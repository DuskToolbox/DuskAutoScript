#ifndef DAS_CORE_IPC_IPC_PERMISSIONS_H
#define DAS_CORE_IPC_IPC_PERMISSIONS_H

#include <boost/interprocess/permissions.hpp>
#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

/// RAII wrapper for boost::interprocess::permissions.
/// On Windows: constructs SECURITY_ATTRIBUTES with current-user-only DACL via
/// SDDL. On POSIX: uses mode_t(0600) (owner read/write).
///
/// Lifecycle: must outlive the IPC object constructor call.
/// After construction, the kernel has copied the security descriptor,
/// and this object can be safely destroyed.
class IpcSecurityAttributes
{
public:
    IpcSecurityAttributes();
    ~IpcSecurityAttributes();

    /// Returns permissions for boost IPC object creation.
    /// Valid only while this object is alive.
    boost::interprocess::permissions GetPermissions() const;

    IpcSecurityAttributes(const IpcSecurityAttributes&) = delete;
    IpcSecurityAttributes& operator=(const IpcSecurityAttributes&) = delete;
    IpcSecurityAttributes(IpcSecurityAttributes&&) = delete;
    IpcSecurityAttributes& operator=(IpcSecurityAttributes&&) = delete;

private:
    boost::interprocess::permissions perm_;

#if defined(_WIN32) || defined(__CYGWIN__)
    void* security_descriptor_{nullptr}; // owns the SECURITY_DESCRIPTOR memory
#endif
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_PERMISSIONS_H
