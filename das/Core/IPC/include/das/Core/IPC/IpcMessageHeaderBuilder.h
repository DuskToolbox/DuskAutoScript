#ifndef DAS_CORE_IPC_IPC_MESSAGE_HEADER_BUILDER_H
#define DAS_CORE_IPC_IPC_MESSAGE_HEADER_BUILDER_H

#include <das/Core/IPC/IpcCommandHandler.h>
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
        header_.interface_id = 0;
        header_.method_id = 0;
        header_.flags = 0;
        header_.error_code = 0;
        header_.body_size = 0;
        header_.session_id = 0;
        header_.generation = 0;
        header_.local_id = 0;
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
    template <ControlPlaneEnum EnumT>
    IPCMessageHeaderBuilder& SetControlPlaneCommand(EnumT command) noexcept
    {
        header_.interface_id = static_cast<uint32_t>(command);
        header_.method_id = 0;
        return *this;
    }

    IPCMessageHeaderBuilder& SetBusinessInterface(
        uint32_t interface_id,
        uint16_t method_id) noexcept
    {
        header_.interface_id = interface_id;
        header_.method_id = method_id;
        return *this;
    }

    IPCMessageHeaderBuilder& SetBodySize(uint32_t size) noexcept
    {
        header_.body_size = size;
        return *this;
    }

    // ==================== 可选字段 ====================

    IPCMessageHeaderBuilder& SetSessionId(uint16_t session_id) noexcept
    {
        header_.session_id = session_id;
        return *this;
    }

    IPCMessageHeaderBuilder& SetCallId(uint64_t call_id) noexcept
    {
        header_.call_id = call_id;
        return *this;
    }

    IPCMessageHeaderBuilder& SetObject(
        uint16_t session_id,
        uint16_t generation,
        uint32_t local_id) noexcept
    {
        header_.session_id = session_id;
        header_.generation = generation;
        header_.local_id = local_id;
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
    uint16_t       session_id = 0)
{
    return IPCMessageHeaderBuilder()
        .SetMessageType(MessageType::REQUEST)
        .SetControlPlaneCommand(command)
        .SetBodySize(body_size)
        .SetSessionId(session_id)
        .Build();
}

[[nodiscard]]
inline ValidatedIPCMessageHeader MakeControlPlaneResponse(
    IpcCommandType command,
    uint32_t       body_size,
    uint64_t       call_id,
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

[[nodiscard]]
inline ValidatedIPCMessageHeader MakeBusinessRequest(
    uint32_t interface_id,
    uint16_t method_id,
    uint32_t body_size,
    uint16_t session_id,
    uint16_t generation,
    uint32_t local_id)
{
    return IPCMessageHeaderBuilder()
        .SetMessageType(MessageType::REQUEST)
        .SetBusinessInterface(interface_id, method_id)
        .SetBodySize(body_size)
        .SetObject(session_id, generation, local_id)
        .Build();
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_MESSAGE_HEADER_BUILDER_H
