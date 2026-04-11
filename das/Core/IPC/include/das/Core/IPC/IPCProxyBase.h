#ifndef DAS_CORE_IPC_IPC_PROXY_BASE_H
#define DAS_CORE_IPC_IPC_PROXY_BASE_H

#include <cstdint>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasGuidHolder.h>
#include <das/IDasBase.h>
#include <memory>
#include <vector>

#include <das/Core/IPC/Config.h>

// {200A224F-5F47-4823-9E51-CC97168B1D1C}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::IPC,
    IPCProxyBase,
    0x200A224F,
    0x5F47,
    0x4823,
    0x9E,
    0x51,
    0xCC,
    0x97,
    0x16,
    0x8B,
    0x1D,
    0x1C);

DAS_CORE_IPC_NS_BEGIN
class BusinessThread;
class DistributedObjectManager;
class ProxyFactory;

class IPCProxyBase
{
public:
    virtual ~IPCProxyBase() = default;

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

    /// @brief 获取分布式对象管理器
    [[nodiscard]]
    DistributedObjectManager& GetObjectManager() const noexcept;

    /// @brief 获取 run loop
    [[nodiscard]]
    IpcRunLoop* GetRunLoop() const noexcept
    {
        return &run_loop_;
    }

    /// @brief 获取业务线程
    [[nodiscard]]
    std::weak_ptr<BusinessThread> GetBusinessThread() const noexcept
    {
        return business_thread_;
    }

    /// @brief 获取 ProxyFactory
    [[nodiscard]]
    ProxyFactory& GetProxyFactory() const noexcept
    {
        return proxy_factory_;
    }

    /// @brief 发送同步请求（PostSend + PumpUntilResponse）
    /// @param method_id 方法 ID（用于日志记录）
    /// @param body 请求体（包含完整的 V3 Body Header）
    /// @param body_size 请求体大小
    /// @param out_response [out] 响应体
    /// @param out_flags [out] 可选：响应头 flags 字段（用于检测 SHM_RESPONSE
    /// 等）
    /// @return DasResult 处理结果
    /// @note body 参数是完整的 V3 消息体（已包含 Body Header）
    DasResult SendRequest(
        uint16_t              method_id,
        const uint8_t*        body,
        size_t                body_size,
        std::vector<uint8_t>& out_response,
        uint16_t*             out_flags = nullptr);

    /// @brief 发送业务控制命令请求（PostSend + PumpUntilResponse）
    /// @param command 命令类型（如 QUERY_INTERFACE）
    /// @param body 请求体
    /// @param body_size 请求体大小
    /// @param out_response [out] 响应体
    /// @return DasResult 处理结果
    /// @note 使用 BUSINESS_CONTROL 路由，Host 侧通过 IpcCommandHandler 分发
    DasResult SendBusinessControlRequest(
        IpcCommandType        command,
        const uint8_t*        body,
        size_t                body_size,
        std::vector<uint8_t>& out_response);

protected:
    /// @brief 构造函数
    /// @param interface_id 接口 ID
    /// @param object_id 对象 ID（用于 body 序列化）
    /// @param run_loop IPC 运行循环（引用）
    /// @param business_thread 业务线程（weak_ptr，用于 PumpUntilResponse）
    /// @param proxy_factory ProxyFactory（引用）
    IPCProxyBase(
        uint32_t                      interface_id,
        const ObjectId&               object_id,
        IpcRunLoop&                   run_loop,
        std::weak_ptr<BusinessThread> business_thread,
        ProxyFactory&                 proxy_factory);

    [[nodiscard]]
    uint16_t NextCallId() noexcept
    {
        return run_loop_.AllocateCallId();
    }

    /// @brief 获取本地 session ID (V3)
    /// @return 本地 session ID
    /// @note 用于设置 Header 的 source_session_id
    [[nodiscard]]
    uint16_t GetSourceSessionId() const noexcept
    {
        return run_loop_.GetSessionId();
    }

    /// @brief 构建业务消息 Body (V3 版本)
    /// @param method_id 方法 ID
    /// @param params 参数字节数据
    /// @param params_size 参数大小
    /// @return 包含 V3 Body Header + 参数的字节向量
    /// @note V3: Body 结构为 interface_id(4B) + method_id(2B) + reserved(2B) +
    /// ObjectId(8B) + 参数
    [[nodiscard]]
    std::vector<uint8_t> BuildBusinessBody(
        uint16_t       method_id,
        const uint8_t* params = nullptr,
        size_t         params_size = 0) const
    {
        std::vector<uint8_t> body;

        // 写入 interface_id (4 bytes)
        body.insert(
            body.end(),
            reinterpret_cast<const uint8_t*>(&interface_id_),
            reinterpret_cast<const uint8_t*>(&interface_id_)
                + sizeof(interface_id_));

        // 写入 method_id (2 bytes)
        body.insert(
            body.end(),
            reinterpret_cast<const uint8_t*>(&method_id),
            reinterpret_cast<const uint8_t*>(&method_id) + sizeof(method_id));

        // 写入 reserved (2 bytes, 值为 0)
        uint16_t reserved = 0;
        body.insert(
            body.end(),
            reinterpret_cast<const uint8_t*>(&reserved),
            reinterpret_cast<const uint8_t*>(&reserved) + sizeof(reserved));

        // 写入 ObjectId (8 bytes)
        body.insert(
            body.end(),
            reinterpret_cast<const uint8_t*>(&object_id_),
            reinterpret_cast<const uint8_t*>(&object_id_) + sizeof(object_id_));

        // 追加参数数据
        if (params != nullptr && params_size > 0)
        {
            body.insert(body.end(), params, params + params_size);
        }

        return body;
    }

    /// @brief 构建请求 header (V3 版本)
    /// @param call_id 调用 ID (16-bit)
    /// @param message_type 消息类型
    /// @param body_size body 大小
    /// @return 构建好的 header
    /// @note V3: Header 负责路由，interface_id 必须设置以便 Host
    ///       DispatchToHandlerCoroutine 找到对应的 stub handler
    [[nodiscard]]
    ValidatedIPCMessageHeader BuildRequestHeader(
        uint16_t    call_id,
        MessageType message_type = MessageType::REQUEST,
        size_t      body_size = 0) const
    {
        return IPCMessageHeaderBuilder()
            .SetMessageType(message_type)
            .SetBodySize(static_cast<uint32_t>(body_size))
            .SetCallId(call_id)
            .SetInterfaceId(interface_id_)
            .SetSourceSessionId(GetSourceSessionId())
            .SetTargetSessionId(object_id_.session_id)
            .Build();
    }

protected:
    ProxyFactory& proxy_factory_; // 引用，生命周期由外部管理

private:
    uint32_t                      interface_id_;
    ObjectId                      object_id_;
    IpcRunLoop&                   run_loop_;        // 引用，生命周期由外部管理
    std::weak_ptr<BusinessThread> business_thread_; // 用于 PumpUntilResponse
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_PROXY_BASE_H
