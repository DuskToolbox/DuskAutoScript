#ifndef DAS_CORE_IPC_QUERY_INTERFACE_STUB_H
#define DAS_CORE_IPC_QUERY_INTERFACE_STUB_H

#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/IpcCommandHandler.h>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief QUERY_INTERFACE 命令的 IPC Stub
 *
 * 处理 IpcCommandType::QUERY_INTERFACE (120) 的远程 QueryInterface 请求。
 * 不继承 IStubBase，因为 body 格式不同（ObjectId + DasGuid，无 V3 Body
 * Header）。
 *
 * Body 格式：
 *   - ObjectId (8B: session_id 2B + generation 2B + local_id 4B)
 *   - DasGuid  (16B)
 *
 * 成功响应格式：
 *   - int32_t result
 *   - uint32_t interface_id (FNV-1a hash of DasGuid)
 *   - uint64_t encoded_object_id
 *
 * 状态无 → 全局单例，永不销毁。
 */
class QueryInterfaceStub final : public IMessageHandler
{
public:
    [[nodiscard]]
    uint32_t AddRef() override
    {
        return std::numeric_limits<uint32_t>::max();
    }

    [[nodiscard]]
    uint32_t Release() override
    {
        return std::numeric_limits<uint32_t>::max();
    }

    [[nodiscard]]
    uint32_t GetInterfaceId() const noexcept override
    {
        return static_cast<uint32_t>(IpcCommandType::QUERY_INTERFACE);
    }

    DasResult HandleMessage(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender,
        StubContext&                     ctx) override;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_QUERY_INTERFACE_STUB_H
