#ifndef DAS_CORE_IPC_IPC_MESSAGE_HEADER_BUILDER_H
#define DAS_CORE_IPC_IPC_MESSAGE_HEADER_BUILDER_H

#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <type_traits>

DAS_CORE_IPC_NS_BEGIN

// ==================== Concept 约束 ====================

/// @brief 控制平面枚举类型约束
/// @note 只接受 IpcCommandType 或 HandshakeInterfaceId
template <typename T>
concept ControlPlaneEnum = std::is_same_v<T, IpcCommandType>
                           || std::is_same_v<T, HandshakeInterfaceId>;

/// @brief IPCMessageHeader 构造器 - 唯一合法的构造方式
class IPCMessageHeaderBuilder
{
public:
    IPCMessageHeaderBuilder() noexcept { Reset(); }

    /// @brief 重置为默认有效值
    IPCMessageHeaderBuilder& Reset() noexcept
    {
        header_ = {};
        header_.magic = IPCMessageHeader::MAGIC;
        header_.version = IPCMessageHeader::CURRENT_VERSION;
        header_.message_type = static_cast<uint8_t>(MessageType::REQUEST);
        header_.header_flags = 0;
        header_.call_id = 0;
        header_.source_session_id = 0;
        header_.target_session_id = 0;
        header_.interface_id = 0;
        header_.flags = 0;
        header_.error_code = 0;
        header_.body_size = 0;
        header_.reserved = 0;
        return *this;
    }

    // ==================== 必须设置的字段 ====================

    IPCMessageHeaderBuilder& SetMessageType(MessageType type) noexcept
    {
        header_.message_type = static_cast<uint8_t>(type);
        return *this;
    }

    /// @brief 设置控制平面命令（类型安全）
    /// @note 只接受 IpcCommandType 或 HandshakeInterfaceId
    /// @note 同时设置 header_flags = CONTROL_PLANE
    template <ControlPlaneEnum EnumT>
    IPCMessageHeaderBuilder& SetControlPlaneCommand(EnumT command) noexcept
    {
        header_.header_flags = HeaderFlags::CONTROL_PLANE;
        header_.interface_id = static_cast<uint32_t>(command);
        return *this;
    }

    /// @brief 设置接口ID
    /// @note 控制平面消息直接使用此方法设置命令类型
    ///       业务消息的 interface_id 可设为 0 或实际值，method_id 在 body 中
    IPCMessageHeaderBuilder& SetInterfaceId(uint32_t interface_id) noexcept
    {
        header_.interface_id = interface_id;
        return *this;
    }

    IPCMessageHeaderBuilder& SetBodySize(uint32_t size) noexcept
    {
        header_.body_size = size;
        return *this;
    }

    // ==================== 可选字段 ====================

    /// @brief 设置消息来源 session ID
    IPCMessageHeaderBuilder& SetSourceSessionId(uint16_t session_id) noexcept
    {
        header_.source_session_id = session_id;
        return *this;
    }

    /// @brief 设置消息目标 session ID
    IPCMessageHeaderBuilder& SetTargetSessionId(uint16_t session_id) noexcept
    {
        header_.target_session_id = session_id;
        return *this;
    }

    /// @brief 设置 call_id (16-bit)
    /// @note V3 Header 使用 16-bit call_id，配合 source_session_id
    /// 匹配请求/响应
    IPCMessageHeaderBuilder& SetCallId(uint16_t call_id) noexcept
    {
        header_.call_id = call_id;
        return *this;
    }

    IPCMessageHeaderBuilder& SetFlags(uint16_t flags) noexcept
    {
        header_.flags = flags;
        return *this;
    }

    IPCMessageHeaderBuilder& SetErrorCode(int32_t error_code) noexcept
    {
        header_.error_code = error_code;
        header_.message_type = static_cast<uint8_t>(MessageType::RESPONSE);
        return *this;
    }

    // ==================== 构建方法 ====================

    [[nodiscard]]
    ValidatedIPCMessageHeader Build() noexcept
    {
        return ValidatedIPCMessageHeader(header_);
    }

private:
    IPCMessageHeader header_;
};

// ==================== 快捷工厂函数 ====================

[[nodiscard]]
inline ValidatedIPCMessageHeader MakeControlPlaneRequest(
    IpcCommandType command,
    uint32_t       body_size,
    uint16_t       target_session_id = 0)
{
    return IPCMessageHeaderBuilder()
        .SetMessageType(MessageType::REQUEST)
        .SetControlPlaneCommand(command)
        .SetBodySize(body_size)
        .SetTargetSessionId(target_session_id)
        .Build();
}

[[nodiscard]]
inline ValidatedIPCMessageHeader MakeControlPlaneResponse(
    IpcCommandType command,
    uint32_t       body_size,
    uint16_t       call_id,
    int32_t        error_code = 0)
{
    return IPCMessageHeaderBuilder()
        .SetMessageType(MessageType::RESPONSE)
        .SetControlPlaneCommand(command)
        .SetBodySize(body_size)
        .SetCallId(call_id)
        .SetErrorCode(error_code)
        .Build();
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_MESSAGE_HEADER_BUILDER_H
