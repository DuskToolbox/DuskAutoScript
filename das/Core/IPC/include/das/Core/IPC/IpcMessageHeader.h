#ifndef DAS_CORE_IPC_IPC_MESSAGE_HEADER_H
#define DAS_CORE_IPC_IPC_MESSAGE_HEADER_H

#include <cstdint>
#include <cstring>
#include <das/IDasBase.h>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        /// @brief IPC 消息类型
        enum class MessageType : uint8_t
        {
            REQUEST = 1,
            RESPONSE = 2,
            EVENT = 3,
            HEARTBEAT = 4
        };

        /// @brief MessageHeader V2 - 40B 无 padding
        /// @note 破坏性变更：完全移除 V1 支持
        struct alignas(8) IPCMessageHeader
        {
            uint32_t magic;        // 'DIPC' (0x43495044) 用于快速验证
            uint16_t version;      // header 版本 (2)
            uint8_t  message_type; // REQUEST/RESPONSE/EVENT/HEARTBEAT
            uint8_t  header_flags; // 预留扩展
            uint64_t call_id;      // request/response 配对

            uint32_t interface_id; // 控制平面：opcode；业务：接口ID
            uint16_t method_id;    // 业务方法ID（控制平面固定 0）
            uint16_t flags;        // bit0=SHM 等
            int32_t  error_code;   // 响应错误码
            uint32_t body_size;    // body 长度

            uint16_t session_id; // 逻辑会话ID
            uint16_t generation; // 对象版本
            uint32_t local_id;   // 本地对象ID

            static constexpr uint32_t MAGIC = 0x43495044; // 'DIPC'
            static constexpr uint16_t CURRENT_VERSION = 2;
        };

        // V2 结构大小验证
        static_assert(
            sizeof(IPCMessageHeader) == 40,
            "IPCMessageHeader must be exactly 40 bytes");
        static_assert(
            alignof(IPCMessageHeader) == 8,
            "IPCMessageHeader alignment must be 8");

    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_IPC_MESSAGE_HEADER_H
