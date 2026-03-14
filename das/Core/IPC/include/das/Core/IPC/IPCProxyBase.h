#ifndef DAS_CORE_IPC_IPC_PROXY_BASE_H
#define DAS_CORE_IPC_IPC_PROXY_BASE_H

#include <cstdint>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/IDasBase.h>
#include <vector>

#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN
class IpcRunLoop;

class IPCProxyBase
{
public:
    virtual ~IPCProxyBase() = default;

    /// @brief 增加引用计数
    /// @return 新的引用计数
    virtual uint32_t AddRef() { return ++refcount_; }

    /// @brief 释放引用计数
    /// @return 新的引用计数
    virtual uint32_t Release()
    {
        if (--refcount_ == 0)
        {
            delete this;
            return 0;
        }
        return refcount_;
    }

    /// @brief 获取对象 ID
    /// @note ObjectId 现在通过 body 传递
    [[nodiscard]]
    const ObjectId& GetObjectId() const noexcept
    {
        return object_id_;
    }

    /// @brief 获取接口 ID
    [[nodiscard]]
    uint32_t GetInterfaceId() const noexcept
    {
        return interface_id_;
    }

    /// @brief 获取 run loop
    [[nodiscard]]
    IpcRunLoop* GetRunLoop() const noexcept
    {
        return run_loop_;
    }

    /// @brief 获取 transport
    [[nodiscard]]
    DefaultAsyncIpcTransport* GetTransport() const noexcept
    {
        return transport_;
    }

protected:
    /// @brief 构造函数
    /// @param interface_id 接口 ID
    /// @param object_id 对象 ID（用于 body 序列化）
    /// @param run_loop IPC 运行循环
    /// @param transport IPC 传输层
    IPCProxyBase(
        uint32_t                  interface_id,
        const ObjectId&           object_id,
        IpcRunLoop*               run_loop,
        DefaultAsyncIpcTransport* transport)
        : interface_id_(interface_id), object_id_(object_id),
          run_loop_(run_loop), transport_(transport), next_call_id_(0)
    {
    }

    /// @brief 生成下一个 call_id (V3: 16-bit)
    /// @note V3 Header 使用 16-bit call_id，配合 source_session_id
    /// 匹配请求/响应
    [[nodiscard]]
    uint16_t NextCallId() noexcept
    {
        // 简单递增，溢出后从 1 开始（0 表示无效）
        if (++next_call_id_ == 0)
        {
            next_call_id_ = 1;
        }
        return static_cast<uint16_t>(next_call_id_);
    }

    /// @brief 构建请求 header (V3 版本)
    /// @param method_id 方法 ID (将在 body 中序列化)
    /// @param call_id 调用 ID (16-bit)
    /// @param message_type 消息类型
    /// @param body_size body 大小
    /// @return 构建好的 header
    /// @note V3: interface_id 在 header 中，method_id 和 ObjectId 在 body 中
    [[nodiscard]]
    ValidatedIPCMessageHeader BuildRequestHeader(
        uint16_t    call_id,
        MessageType message_type = MessageType::REQUEST,
        size_t      body_size = 0) const
    {
        return IPCMessageHeaderBuilder()
            .SetMessageType(message_type)
            .SetInterfaceId(interface_id_)
            .SetBodySize(static_cast<uint32_t>(body_size))
            .SetCallId(call_id)
            .SetTargetSessionId(object_id_.session_id)
            .Build();
    }

private:
    uint32_t                  interface_id_;
    ObjectId                  object_id_;
    IpcRunLoop*               run_loop_;
    DefaultAsyncIpcTransport* transport_;
    uint16_t                  next_call_id_{0}; // V3: 16-bit call_id
    uint32_t                  refcount_{1};     // 初始引用计数为1（创建时）
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_PROXY_BASE_H
