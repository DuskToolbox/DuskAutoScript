#ifndef DAS_CORE_IPC_I_HOST_CONNECTION_H
#define DAS_CORE_IPC_I_HOST_CONNECTION_H

#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/Config.h>
#include <das/IDasBase.h>

// {72F00101-9B7C-4E67-9B01-4F94445A7201}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::IPC,
    IHostConnection,
    0x72F00101,
    0x9B7C,
    0x4E67,
    0x9B,
    0x01,
    0x4F,
    0x94,
    0x44,
    0x5A,
    0x72,
    0x01);

DAS_CORE_IPC_NS_BEGIN

class IHostConnection : public IDasBase
{
public:
    virtual TransportLookupResult    GetTransport() = 0;
    virtual boost::asio::io_context& GetIoContext() = 0;
    virtual uint16_t                 GetSessionId() const = 0;
    virtual uint32_t                 GetPid() const = 0;
    virtual bool                     IsRunning() const = 0;
    virtual void                     Stop() = 0;
    virtual void                     ClearCallbacks() = 0;
    virtual void                     NotifyHeartbeatTimeout() = 0;
    virtual void                     TerminateIfRunning() = 0;

protected:
    IHostConnection() = default;
    virtual ~IHostConnection() = default;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_I_HOST_CONNECTION_H
