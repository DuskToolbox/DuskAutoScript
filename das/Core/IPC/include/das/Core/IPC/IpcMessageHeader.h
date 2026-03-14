#ifndef DAS_CORE_IPC_IPC_MESSAGE_HEADER_H
#define DAS_CORE_IPC_IPC_MESSAGE_HEADER_H

#include <cstdint>
#include <cstring>
#include <das/IDasBase.h>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

/// @brief 消息头标志
namespace HeaderFlags
{
    /// 无标志
    constexpr uint8_t NONE = 0x00;
    /// 控制平面消息标志
    constexpr uint8_t CONTROL_PLANE = 0x01;
} // namespace HeaderFlags

/// @brief IPC 消息类型
enum class MessageType : uint8_t
{
    REQUEST = 1,
    RESPONSE = 2,
    EVENT = 3,
    HEARTBEAT = 4
};

/// @brief MessageHeader V3 - 32B 紧凑设计
/// @details V3 Header 专为跨进程转发设计:
/// - 移除了 ObjectId 字段 (session_id, generation, local_id) -> 移至 body
/// - 新增 source_session_id / target_session_id 用于路由
/// - call_id 从 64-bit 缩减到 16-bit (配合 source_session_id 匹配)
/// - 保留 interface_id 用于控制平面消息
/// - 修正 MAGIC 为正确的 "DIPC" 小端序
struct alignas(8) IPCMessageHeader
{
    // === 第一组: 8 bytes ===
    uint32_t magic;        // 'DIPC' (0x43504944) 用于快速验证
    uint16_t version;      // header 版本 (3)
    uint8_t  message_type; // REQUEST/RESPONSE/EVENT/HEARTBEAT
    uint8_t  header_flags; // 预留扩展（指示是业务消息还是控制平面消息）

    // === 第二组: 8 bytes ===
    uint16_t call_id;           // request/response 配对 (16-bit)
    uint16_t source_session_id; // 消息来源 session (用于路由)
    uint16_t target_session_id; // 消息目标 session (用于路由)
    uint16_t flags;             // bit0=SHM 等

    // === 第三组: 8 bytes ===
    uint32_t interface_id; // 控制平面：命令类型；业务：接口ID（或 0）
    int32_t  error_code;   // 响应错误码

    // === 第四组: 8 bytes ===
    uint32_t body_size; // body 长度
    uint32_t reserved;  // 保留（未来扩展）

    static constexpr uint32_t MAGIC = 0x43504944; // 'DIPC' (正确的字节序)
    static constexpr uint16_t CURRENT_VERSION = 3;

    /// @brief 验证消息头是否有效
    [[nodiscard]]
    bool IsValid() const noexcept
    {
        return magic == MAGIC && version == CURRENT_VERSION;
    }
};

// V3 结构大小验证
static_assert(
    sizeof(IPCMessageHeader) == 32,
    "IPCMessageHeader V3 must be exactly 32 bytes");
static_assert(
    alignof(IPCMessageHeader) == 8,
    "IPCMessageHeader alignment must be 8");

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_MESSAGE_HEADER_H
