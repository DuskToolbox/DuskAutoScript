#ifndef DAS_CORE_IPC_IPC_HEADER_VALIDATOR_H
#define DAS_CORE_IPC_IPC_HEADER_VALIDATOR_H

#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <source_location>
#include <string_view>

DAS_CORE_IPC_NS_BEGIN

/// @brief Header 验证错误类型
enum class HeaderValidationError : uint8_t
{
    None = 0,

    // 基础校验
    InvalidMagic,
    InvalidVersion,

    // 消息类型校验
    InvalidMessageType,
    UnexpectedMessageType,

    // 控制平面校验
    InvalidControlPlaneCommand,

    // 业务平面校验

    // Body 校验
    BodySizeTooSmall,
    BodySizeExceedsLimit,
    BodySizeMismatch,

    // Session 校验
    InvalidTargetSessionId,

    // 调用校验
    UnknownCallId,
};

/// @brief 调用点信息（用于日志）
struct ValidationCallSite
{
    const char* file = nullptr;
    const char* function = nullptr;
    int         line = 0;

    [[nodiscard]]
    bool IsValid() const noexcept
    {
        return file != nullptr;
    }
};

/// @brief Header 验证结果
struct HeaderValidationResult
{
    HeaderValidationError error = HeaderValidationError::None;
    const char*           message = nullptr;
    ValidationCallSite    call_site; // 调用点信息

    [[nodiscard]]
    bool IsOk() const noexcept
    {
        return error == HeaderValidationError::None;
    }

    [[nodiscard]]
    explicit operator bool() const noexcept
    {
        return IsOk();
    }

    [[nodiscard]]
    const char* GetErrorName() const noexcept;
};

/// @brief 验证上下文
struct HeaderValidationContext
{
    size_t      actual_body_size = 0;
    size_t      min_body_size = 0;
    size_t      max_body_size = 64 * 1024;
    bool        check_session_registered = false;
    bool        check_call_pending = false;
    MessageType expected_type = static_cast<MessageType>(0xFF);

    /// @brief 检查 call_id 是否在 pending 列表中（函数指针）
    /// @param call_id 要检查的调用 ID (V3: uint16_t)
    /// @param user_data 用户数据（通常是指向 IpcRunLoop 的指针）
    /// @return true 如果 call_id 正在等待响应
    bool (*is_call_pending)(uint16_t call_id, void* user_data) = nullptr;
    void* is_call_pending_user_data = nullptr; // 传递给回调的用户数据
};

/// @brief IPC Header 验证器 - 集中所有验证逻辑
class IpcHeaderValidator
{
public:
    // ==================== 核心验证方法（带调用点） ====================

    /// @brief 快速验证 - 仅检查 magic 和 version
    /// @note 用于反序列化后的第一时间校验，性能优先
    [[nodiscard]]
    static HeaderValidationResult QuickValidate(
        const IPCMessageHeader& header,
        std::source_location    loc = std::source_location::current()) noexcept
    {
        if (header.magic != IPCMessageHeader::MAGIC)
        {
            return {
                HeaderValidationError::InvalidMagic,
                "Invalid magic number",
                ValidationCallSite{
                    loc.file_name(),
                    loc.function_name(),
                    static_cast<int>(loc.line())}};
        }

        if (header.version != IPCMessageHeader::CURRENT_VERSION)
        {
            return {
                HeaderValidationError::InvalidVersion,
                "Unsupported version",
                ValidationCallSite{
                    loc.file_name(),
                    loc.function_name(),
                    static_cast<int>(loc.line())}};
        }

        return {};
    }

    /// @brief 完整验证 - 用于消息处理前
    [[nodiscard]]
    static HeaderValidationResult FullValidate(
        const IPCMessageHeader&        header,
        const HeaderValidationContext& context = {},
        std::source_location loc = std::source_location::current()) noexcept
    {
        // 1. 基础校验
        auto result = QuickValidate(header, loc);
        if (!result.IsOk())
        {
            return result;
        }

        // 2. 消息类型校验
        result = ValidateMessageType(header, context, loc);
        if (!result.IsOk())
        {
            return result;
        }

        // 3. 平面校验
        result = ValidatePlane(header, loc);
        if (!result.IsOk())
        {
            return result;
        }

        // 4. Body 大小校验
        result = ValidateBodySize(header, context, loc);
        if (!result.IsOk())
        {
            return result;
        }

        // 5. Session 校验
        if (context.check_session_registered)
        {
            result = ValidateSession(header, loc);
            if (!result.IsOk())
            {
                return result;
            }
        }

        // 6. Call ID 校验
        if (context.check_call_pending)
        {
            result = ValidateCallId(header, context, loc);
            if (!result.IsOk())
            {
                return result;
            }
        }

        return {};
    }

    // ==================== 分类验证方法 ====================

    [[nodiscard]]
    static HeaderValidationResult ValidateMessageType(
        const IPCMessageHeader&        header,
        const HeaderValidationContext& context,
        std::source_location           loc) noexcept
    {
        const auto type = static_cast<MessageType>(header.message_type);

        if (type != MessageType::Request && type != MessageType::Response)
        {
            return {
                HeaderValidationError::InvalidMessageType,
                "Invalid message type",
                ValidationCallSite{
                    loc.file_name(),
                    loc.function_name(),
                    static_cast<int>(loc.line())}};
        }

        if (context.expected_type != static_cast<MessageType>(0xFF)
            && type != context.expected_type)
        {
            return {
                HeaderValidationError::UnexpectedMessageType,
                "Unexpected message type",
                ValidationCallSite{
                    loc.file_name(),
                    loc.function_name(),
                    static_cast<int>(loc.line())}};
        }

        return {};
    }

    [[nodiscard]]
    static HeaderValidationResult ValidatePlane(
        const IPCMessageHeader& header,
        std::source_location    loc) noexcept
    {
        const bool is_control_plane = IsControlPlane(header);

        if (is_control_plane)
        {
            if (!IsValidControlPlaneCommand(header.interface_id))
            {
                return {
                    HeaderValidationError::InvalidControlPlaneCommand,
                    "Invalid control plane command",
                    ValidationCallSite{
                        loc.file_name(),
                        loc.function_name(),
                        static_cast<int>(loc.line())}};
            }
        }

        return {};
    }

    [[nodiscard]]
    static HeaderValidationResult ValidateBodySize(
        const IPCMessageHeader&        header,
        const HeaderValidationContext& context,
        std::source_location           loc) noexcept
    {
        if (context.actual_body_size > 0
            && header.body_size != context.actual_body_size)
        {
            return {
                HeaderValidationError::BodySizeMismatch,
                "Body size mismatch",
                ValidationCallSite{
                    loc.file_name(),
                    loc.function_name(),
                    static_cast<int>(loc.line())}};
        }

        if (context.min_body_size > 0
            && header.body_size < context.min_body_size)
        {
            return {
                HeaderValidationError::BodySizeTooSmall,
                "Body size too small for message type",
                ValidationCallSite{
                    loc.file_name(),
                    loc.function_name(),
                    static_cast<int>(loc.line())}};
        }

        if (header.body_size > context.max_body_size)
        {
            return {
                HeaderValidationError::BodySizeExceedsLimit,
                "Body size exceeds limit",
                ValidationCallSite{
                    loc.file_name(),
                    loc.function_name(),
                    static_cast<int>(loc.line())}};
        }

        return {};
    }

    [[nodiscard]]
    static HeaderValidationResult ValidateSession(
        const IPCMessageHeader&               header,
        [[maybe_unused]] std::source_location loc) noexcept
    {
        // V3: 使用 source_session_id / target_session_id
        // 控制平面消息允许 session_id 为 0
        if (IsControlPlane(header))
        {
            return {};
        }

        // 业务消息需要有效的 target_session_id
        if (header.target_session_id == 0)
        {
            return {
                HeaderValidationError::InvalidTargetSessionId,
                "Invalid target session ID",
                ValidationCallSite{
                    loc.file_name(),
                    loc.function_name(),
                    static_cast<int>(loc.line())}};
        }

        return {};
    }

    [[nodiscard]]
    static HeaderValidationResult ValidateCallId(
        const IPCMessageHeader&        header,
        const HeaderValidationContext& context,
        std::source_location           loc) noexcept
    {
        // 仅对响应消息检查
        if (static_cast<MessageType>(header.message_type)
            != MessageType::Response)
        {
            return {};
        }

        // 如果提供了检查回调，使用它
        if (context.is_call_pending)
        {
            if (!context.is_call_pending(
                    header.call_id,
                    context.is_call_pending_user_data))
            {
                return {
                    HeaderValidationError::UnknownCallId,
                    "Unknown call ID in response",
                    ValidationCallSite{
                        loc.file_name(),
                        loc.function_name(),
                        static_cast<int>(loc.line())}};
            }
        }

        return {};
    }

    // ==================== Call ID 校验使用示例 ====================
    //
    // // 定义静态回调函数（IPC 是顺序无锁的，不需要加锁）
    // static bool IsCallPendingCallback(uint16_t call_id, void* user_data)
    // {
    //     auto* run_loop = static_cast<IpcRunLoop*>(user_data);
    //     return run_loop->pending_calls_.find(call_id) !=
    //     run_loop->pending_calls_.end();
    // }
    //
    // // 使用时设置 context
    // HeaderValidationContext context;
    // context.check_call_pending       = true;
    // context.is_call_pending          = IsCallPendingCallback;
    // context.is_call_pending_user_data = this;  // IpcRunLoop 指针
    //
    // auto result = IpcHeaderValidator::FullValidate(header.Raw(), context);

    // ==================== 辅助方法 ====================

    /// @brief 判断是否为控制平面消息
    /// @note V3: 使用 header_flags 判断，不再依赖 ObjectId
    [[nodiscard]]
    static bool IsControlPlane(const IPCMessageHeader& header) noexcept
    {
        // V3: 控制平面消息由 header_flags 标识
        return (header.header_flags & HeaderFlags::CONTROL_PLANE) != 0;
    }

    [[nodiscard]]
    static bool IsValidControlPlaneCommand(uint32_t interface_id) noexcept
    {
        return interface_id >= static_cast<uint32_t>(
                   HandshakeInterfaceId::HandshakeIfaceHello)
               && interface_id <= static_cast<uint32_t>(
                      HandshakeInterfaceId::HandshakeIfaceGoodbye);
    }
};

// ==================== 错误名称映射 ====================

[[nodiscard]]
inline const char* HeaderValidationResult::GetErrorName() const noexcept
{
    switch (error)
    {
    case HeaderValidationError::None:
        return "NONE";
    case HeaderValidationError::InvalidMagic:
        return "INVALID_MAGIC";
    case HeaderValidationError::InvalidVersion:
        return "INVALID_VERSION";
    case HeaderValidationError::InvalidMessageType:
        return "INVALID_MESSAGE_TYPE";
    case HeaderValidationError::UnexpectedMessageType:
        return "UNEXPECTED_MESSAGE_TYPE";
    case HeaderValidationError::InvalidControlPlaneCommand:
        return "INVALID_CONTROL_PLANE_COMMAND";
    case HeaderValidationError::BodySizeTooSmall:
        return "BODY_SIZE_TOO_SMALL";
    case HeaderValidationError::BodySizeExceedsLimit:
        return "BODY_SIZE_EXCEEDS_LIMIT";
    case HeaderValidationError::BodySizeMismatch:
        return "BODY_SIZE_MISMATCH";
    case HeaderValidationError::InvalidTargetSessionId:
        return "INVALID_TARGET_SESSION_ID";
    case HeaderValidationError::UnknownCallId:
        return "UNKNOWN_CALL_ID";
    default:
        return "UNKNOWN";
    }
}
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_HEADER_VALIDATOR_H
