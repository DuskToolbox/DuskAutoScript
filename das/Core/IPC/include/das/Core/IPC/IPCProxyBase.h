#ifndef DAS_CORE_IPC_IPC_PROXY_BASE_H
#define DAS_CORE_IPC_IPC_PROXY_BASE_H

#include <cstdint>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/IDasBase.h>
#include <vector>

#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/AsyncIpcTransport.h>

DAS_CORE_IPC_NS_BEGIN
class IpcRunLoop;

class IPCProxyBase
{
public:
    virtual ~IPCProxyBase() = default;

    virtual uint32_t AddRef() = 0;
    virtual uint32_t Release() = 0;

    [[nodiscard]]
    uint32_t GetInterfaceId() const noexcept
    {
        return interface_id_;
    }

    [[nodiscard]]
    uint64_t GetObjectId() const noexcept
    {
        return EncodeObjectId(object_id_);
    }

    [[nodiscard]]
    const ObjectId& GetObjectIdStruct() const noexcept
    {
        return object_id_;
    }

    [[nodiscard]]
    uint16_t GetSessionId() const noexcept
    {
        return object_id_.session_id;
    }

    /// 设置 transport（可选，用于事件发送）
    void SetTransport(DefaultAsyncIpcTransport* transport)
    {
        transport_ = transport;
    }

protected:
    IPCProxyBase(
        uint32_t        interface_id,
        const ObjectId& object_id,
        IpcRunLoop*    run_loop)
        : interface_id_(interface_id), object_id_(object_id),
          run_loop_(run_loop), transport_(nullptr), next_call_id_(1)
    {
    }

    DasResult SendRequest(
        uint16_t              method_id,
        const uint8_t*        body,
        size_t                body_size,
        std::vector<uint8_t>& response_body);

    DasResult SendRequestNoResponse(
        uint16_t       method_id,
        const uint8_t* body,
        size_t         body_size);

    [[nodiscard]]
    uint64_t AllocateCallId() noexcept
    {
        return next_call_id_++;
    }

    [[nodiscard]]
    IpcRunLoop* GetRunLoop() const noexcept
    {
        return run_loop_;
    }

    [[nodiscard]]
    DefaultAsyncIpcTransport* GetTransport() const noexcept
    {
        return transport_;
    }

    [[nodiscard]]
    ValidatedIPCMessageHeader BuildMessageHeader(
        uint16_t    method_id,
        uint64_t    call_id,
        MessageType message_type = MessageType::REQUEST,
        size_t      body_size = 0) const
    {
        return IPCMessageHeaderBuilder()
            .SetMessageType(message_type)
            .SetBusinessInterface(interface_id_, method_id)
            .SetBodySize(static_cast<uint32_t>(body_size))
            .SetCallId(call_id)
            .SetObject(
                object_id_.session_id,
                object_id_.generation,
                object_id_.local_id)
            .Build();
    }

private:
    uint32_t                   interface_id_;
    ObjectId                   object_id_;
    IpcRunLoop*                run_loop_;
    DefaultAsyncIpcTransport*  transport_;
    uint64_t                   next_call_id_;
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_PROXY_BASE_H
