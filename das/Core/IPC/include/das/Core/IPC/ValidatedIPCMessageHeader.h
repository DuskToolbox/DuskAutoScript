#ifndef DAS_CORE_IPC_VALIDATED_IPC_MESSAGE_HEADER_H
#define DAS_CORE_IPC_VALIDATED_IPC_MESSAGE_HEADER_H

#include <cstring>
#include <das/Core/IPC/IpcHeaderValidator.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <optional>

DAS_CORE_IPC_NS_BEGIN

/// @brief 经过验证的消息头 - 只能通过 Builder 创建
/// @note 这是一个编译时保护壳，零运行时开销
class ValidatedIPCMessageHeader
{
public:
    [[nodiscard]]
    const IPCMessageHeader& Raw() const noexcept
    {
        return header_;
    }

    /// @brief 隐式转换到 const IPCMessageHeader*（用于发送）
    [[nodiscard]]
    operator const IPCMessageHeader*() const noexcept
    {
        return &header_;
    }

    /// @brief 获取大小（用于序列化）
    [[nodiscard]]
    static constexpr size_t Size() noexcept
    {
        return sizeof(IPCMessageHeader);
    }

    /// @brief 从原始字节反序列化并验证
    [[nodiscard]]
    static std::optional<ValidatedIPCMessageHeader> Deserialize(
        const void*             data,
        size_t                  size,
        HeaderValidationResult* out_error = nullptr)
    {
        if (size < sizeof(IPCMessageHeader))
        {
            if (out_error)
            {
                *out_error = {
                    HeaderValidationError::BODY_SIZE_TOO_SMALL,
                    "Data too small"};
            }
            return std::nullopt;
        }

        IPCMessageHeader raw;
        std::memcpy(&raw, data, sizeof(raw));

        auto result = IpcHeaderValidator::QuickValidate(raw);
        if (!result.IsOk())
        {
            if (out_error)
            {
                *out_error = result;
            }
            return std::nullopt;
        }

        return ValidatedIPCMessageHeader(raw);
    }

    // 默认拷贝/移动
    ValidatedIPCMessageHeader(const ValidatedIPCMessageHeader&) = default;
    ValidatedIPCMessageHeader& operator=(const ValidatedIPCMessageHeader&) =
        default;
    ValidatedIPCMessageHeader(ValidatedIPCMessageHeader&&) = default;
    ValidatedIPCMessageHeader& operator=(ValidatedIPCMessageHeader&&) = default;

    // ==================== 业务字段 Getter ====================

    /// @brief 获取消息类型
    [[nodiscard]]
    MessageType GetMessageType() const noexcept
    {
        return static_cast<MessageType>(header_.message_type);
    }

    /// @brief 获取 Header 标志
    [[nodiscard]]
    uint8_t GetHeaderFlags() const noexcept
    {
        return header_.header_flags;
    }

    /// @brief 获取调用 ID (用于请求/响应配对)
    [[nodiscard]]
    uint16_t GetCallId() const noexcept
    {
        return header_.call_id;
    }

    /// @brief 获取源 Session ID
    [[nodiscard]]
    uint16_t GetSourceSessionId() const noexcept
    {
        return header_.source_session_id;
    }

    /// @brief 获取目标 Session ID
    [[nodiscard]]
    uint16_t GetTargetSessionId() const noexcept
    {
        return header_.target_session_id;
    }

    /// @brief 获取接口 ID
    [[nodiscard]]
    uint32_t GetInterfaceId() const noexcept
    {
        return header_.interface_id;
    }

    /// @brief 获取错误码
    [[nodiscard]]
    int32_t GetErrorCode() const noexcept
    {
        return header_.error_code;
    }

    /// @brief 获取 Body 大小
    [[nodiscard]]
    uint32_t GetBodySize() const noexcept
    {
        return header_.body_size;
    }

    /// @brief 获取扩展标志
    [[nodiscard]]
    uint16_t GetFlags() const noexcept
    {
        return header_.flags;
    }

    // ==================== 类型判断便捷方法 ====================

    /// @brief 是否为请求消息
    [[nodiscard]]
    bool IsRequest() const noexcept
    {
        return GetMessageType() == MessageType::REQUEST;
    }

    /// @brief 是否为响应消息
    [[nodiscard]]
    bool IsResponse() const noexcept
    {
        return GetMessageType() == MessageType::RESPONSE;
    }

    /// @brief 是否为事件消息
    [[nodiscard]]
    bool IsEvent() const noexcept
    {
        return GetMessageType() == MessageType::EVENT;
    }

    /// @brief 是否为心跳消息
    [[nodiscard]]
    bool IsHeartbeat() const noexcept
    {
        return GetMessageType() == MessageType::HEARTBEAT;
    }

    /// @brief 是否为控制平面消息
    [[nodiscard]]
    bool IsControlPlane() const noexcept
    {
        return (GetHeaderFlags() & HeaderFlags::CONTROL_PLANE) != 0;
    }

    /// @brief 是否为业务平面消息
    [[nodiscard]]
    bool IsBusinessPlane() const noexcept
    {
        return !IsControlPlane();
    }

    /// @brief 是否有错误
    [[nodiscard]]
    bool HasError() const noexcept
    {
        return GetErrorCode() != 0;
    }

    /// @brief 是否成功 (仅对响应消息有意义)
    [[nodiscard]]
    bool IsSuccess() const noexcept
    {
        return GetErrorCode() == 0;
    }

private:
    IPCMessageHeader header_;

    friend class IPCMessageHeaderBuilder;
    friend class IpcTransport;
    friend class MessageDeserializer;

    explicit ValidatedIPCMessageHeader(const IPCMessageHeader& header) noexcept
        : header_(header)
    {
        // Builder 已确保 header 有效，无需运行时断言
    }
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_VALIDATED_IPC_MESSAGE_HEADER_H
