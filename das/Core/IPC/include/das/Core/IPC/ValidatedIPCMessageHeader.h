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
    /// @brief 获取原始 header（只读）
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
