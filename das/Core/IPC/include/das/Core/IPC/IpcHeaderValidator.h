#ifndef DAS_CORE_IPC_IPC_HEADER_VALIDATOR_H
#define DAS_CORE_IPC_IPC_HEADER_VALIDATOR_H

#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <source_location>
#include <string_view>

DAS_CORE_IPC_NS_BEGIN

/// @brief Header 验证错误类型
enum class HeaderValidationError : uint8_t
{
    NONE = 0,

    // 基础校验
    INVALID_MAGIC,
    INVALID_VERSION,

    // 消息类型校验
    INVALID_MESSAGE_TYPE,
    UNEXPECTED_MESSAGE_TYPE,

    // 控制平面校验
    INVALID_CONTROL_PLANE_COMMAND,
    CONTROL_PLANE_METHOD_ID_NOT_ZERO,

    // 业务平面校验
    BUSINESS_PLANE_INVALID_OBJECT_ID,

    // Body 校验
    BODY_SIZE_TOO_SMALL,
    BODY_SIZE_EXCEEDS_LIMIT,
    BODY_SIZE_MISMATCH,

    // Session 校验
    INVALID_SESSION_ID,
    SESSION_NOT_REGISTERED,

    // 调用校验
    UNKNOWN_CALL_ID,

    // 标志校验
    INVALID_FLAGS,
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
    HeaderValidationError error = HeaderValidationError::NONE;
    const char*           message = nullptr;
    ValidationCallSite    call_site; // 调用点信息

    [[nodiscard]]
    bool IsOk() const noexcept
    {
        return error == HeaderValidationError::NONE;
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
    /// @param call_id 要检查的调用 ID
    /// @param user_data 用户数据（通常是指向 IpcRunLoop 的指针）
    /// @return true 如果 call_id 正在等待响应
    bool (*is_call_pending)(uint64_t call_id, void* user_data) = nullptr;
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
                HeaderValidationError::INVALID_MAGIC,
                "Invalid magic number",
                ValidationCallSite{
                    loc.file_name(),
                    loc.function_name(),
                    static_cast<int>(loc.line())}};
        }

        if (header.version != IPCMessageHeader::CURRENT_VERSION)
        {
            return {
                HeaderValidationError::INVALID_VERSION,
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

        if (type != MessageType::REQUEST && type != MessageType::RESPONSE)
        {
            return {
                HeaderValidationError::INVALID_MESSAGE_TYPE,
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
                HeaderValidationError::UNEXPECTED_MESSAGE_TYPE,
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
            if (header.method_id != 0)
            {
                return {
                    HeaderValidationError::CONTROL_PLANE_METHOD_ID_NOT_ZERO,
                    "Control plane message must have method_id = 0",
                    ValidationCallSite{
                        loc.file_name(),
                        loc.function_name(),
                        static_cast<int>(loc.line())}};
            }

            if (!IsValidControlPlaneCommand(header.interface_id))
            {
                return {
                    HeaderValidationError::INVALID_CONTROL_PLANE_COMMAND,
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
                HeaderValidationError::BODY_SIZE_MISMATCH,
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
                HeaderValidationError::BODY_SIZE_TOO_SMALL,
                "Body size too small for message type",
                ValidationCallSite{
                    loc.file_name(),
                    loc.function_name(),
                    static_cast<int>(loc.line())}};
        }

        if (header.body_size > context.max_body_size)
        {
            return {
                HeaderValidationError::BODY_SIZE_EXCEEDS_LIMIT,
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
        if (header.session_id == 0)
        {
            return {};
        }

        // TODO: 实现实际的 session 验证
        // if (!SessionCoordinator::IsValidSessionId(header.session_id))
        // {
        //     return {
        //         HeaderValidationError::INVALID_SESSION_ID,
        //         "Invalid session ID",
        //         ValidationCallSite{loc.file_name(), loc.function_name(),
        //                            static_cast<int>(loc.line())}};
        // }

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
            != MessageType::RESPONSE)
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
                    HeaderValidationError::UNKNOWN_CALL_ID,
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
    // static bool IsCallPendingCallback(uint64_t call_id, void* user_data)
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

    [[nodiscard]]
    static bool IsControlPlane(const IPCMessageHeader& header) noexcept
    {
        return header.interface_id >= static_cast<uint32_t>(
                   HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO)
               && header.interface_id <= static_cast<uint32_t>(
                      HandshakeInterfaceId::HANDSHAKE_IFACE_GOODBYE);
    }

    [[nodiscard]]
    static bool IsHandshakeMessage(const IPCMessageHeader& header) noexcept
    {
        return IsControlPlane(header);
    }

    [[nodiscard]]
    static bool IsValidControlPlaneCommand(uint32_t interface_id) noexcept
    {
        return interface_id >= static_cast<uint32_t>(
                   HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO)
               && interface_id <= static_cast<uint32_t>(
                      HandshakeInterfaceId::HANDSHAKE_IFACE_GOODBYE);
    }
};

// ==================== 错误名称映射 ====================

[[nodiscard]]
inline const char* HeaderValidationResult::GetErrorName() const noexcept
{
    switch (error)
    {
    case HeaderValidationError::NONE:
        return "NONE";
    case HeaderValidationError::INVALID_MAGIC:
        return "INVALID_MAGIC";
    case HeaderValidationError::INVALID_VERSION:
        return "INVALID_VERSION";
    case HeaderValidationError::INVALID_MESSAGE_TYPE:
        return "INVALID_MESSAGE_TYPE";
    case HeaderValidationError::UNEXPECTED_MESSAGE_TYPE:
        return "UNEXPECTED_MESSAGE_TYPE";
    case HeaderValidationError::INVALID_CONTROL_PLANE_COMMAND:
        return "INVALID_CONTROL_PLANE_COMMAND";
    case HeaderValidationError::CONTROL_PLANE_METHOD_ID_NOT_ZERO:
        return "CONTROL_PLANE_METHOD_ID_NOT_ZERO";
    case HeaderValidationError::BUSINESS_PLANE_INVALID_OBJECT_ID:
        return "BUSINESS_PLANE_INVALID_OBJECT_ID";
    case HeaderValidationError::BODY_SIZE_TOO_SMALL:
        return "BODY_SIZE_TOO_SMALL";
    case HeaderValidationError::BODY_SIZE_EXCEEDS_LIMIT:
        return "BODY_SIZE_EXCEEDS_LIMIT";
    case HeaderValidationError::BODY_SIZE_MISMATCH:
        return "BODY_SIZE_MISMATCH";
    case HeaderValidationError::INVALID_SESSION_ID:
        return "INVALID_SESSION_ID";
    case HeaderValidationError::SESSION_NOT_REGISTERED:
        return "SESSION_NOT_REGISTERED";
    case HeaderValidationError::UNKNOWN_CALL_ID:
        return "UNKNOWN_CALL_ID";
    case HeaderValidationError::INVALID_FLAGS:
        return "INVALID_FLAGS";
    default:
        return "UNKNOWN";
    }
}
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_HEADER_VALIDATOR_H
