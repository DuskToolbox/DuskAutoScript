#ifndef DAS_CORE_IPC_HANDSHAKE_H
#define DAS_CORE_IPC_HANDSHAKE_H

#include <cstdint>
#include <cstring>
#include <das/IDasBase.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        /**
         * @brief 控制平面 interface_id 枚举
         *
         * 根据 B3.1 握手规范，控制平面消息的特征：
         * - object_id = 0
         * - type_id = 0
         * - interface_id 用于区分不同的控制消息
         */
        enum class HandshakeInterfaceId : uint32_t
        {
            HandshakeHello = 1, ///< HelloRequestV1 / WelcomeResponseV1
            HandshakeReady = 2, ///< ReadyRequestV1 / ReadyAckV1
            Heartbeat = 3,      ///< HeartbeatV1
            Goodbye = 4         ///< GoodbyeV1
        };

        /**
         * @brief Goodbye 原因枚举
         */
        enum class GoodbyeReason : uint32_t
        {
            NormalShutdown = 0,    ///< 正常关闭
            HeartbeatTimeout = 1,  ///< 心跳超时
            ProtocolError = 2,     ///< 协议错误
            ResourceExhausted = 3, ///< 资源耗尽
            RequestedByPeer = 4    ///< 对端请求
        };

        /**
         * @brief HelloRequestV1: Child → Host（请求连接）
         *
         * 控制平面消息：
         * - object_id = 0
         * - type_id = 0
         * - interface_id = HandshakeInterfaceId::HandshakeHello
         */
        struct alignas(8) HelloRequestV1
        {
            uint32_t protocol_version; ///< 协议版本（当前为 1）
            uint32_t pid;              ///< Child 进程 ID
            char     plugin_name[64];  ///< 插件名称（UTF-8，null-terminated）

            static constexpr uint32_t CURRENT_PROTOCOL_VERSION = 1;
            static constexpr size_t   PLUGIN_NAME_SIZE = 64;
        };

        /**
         * @brief WelcomeResponseV1: Host → Child（分配 session_id）
         *
         * 控制平面消息：
         * - object_id = 0
         * - type_id = 0
         * - interface_id = HandshakeInterfaceId::HandshakeHello
         */
        struct alignas(8) WelcomeResponseV1
        {
            uint16_t session_id; ///< 分配的 session_id（0 表示失败）
            uint16_t reserved;   ///< 保留字段
            uint32_t status;     ///< 状态码（0 = 成功）

            static constexpr uint32_t STATUS_SUCCESS = 0;
            static constexpr uint32_t STATUS_VERSION_MISMATCH = 1;
            static constexpr uint32_t STATUS_TOO_MANY_CLIENTS = 2;
            static constexpr uint32_t STATUS_INVALID_NAME = 3;
        };

        /**
         * @brief ReadyRequestV1: Child → Host（确认就绪）
         *
         * 控制平面消息：
         * - object_id = 0
         * - type_id = 0
         * - interface_id = HandshakeInterfaceId::HandshakeReady
         */
        struct alignas(8) ReadyRequestV1
        {
            uint16_t session_id; ///< 已分配的 session_id
            uint16_t reserved;   ///< 保留字段
        };

        /**
         * @brief ReadyAckV1: Host → Child（确认）
         *
         * 控制平面消息：
         * - object_id = 0
         * - type_id = 0
         * - interface_id = HandshakeInterfaceId::HandshakeReady
         */
        struct alignas(8) ReadyAckV1
        {
            uint32_t status; ///< 状态码（0 = 成功）

            static constexpr uint32_t STATUS_SUCCESS = 0;
            static constexpr uint32_t STATUS_INVALID_SESSION = 1;
            static constexpr uint32_t STATUS_SESSION_NOT_READY = 2;
        };

        /**
         * @brief HeartbeatV1: 双向（心跳）
         *
         * 控制平面消息：
         * - object_id = 0
         * - type_id = 0
         * - interface_id = HandshakeInterfaceId::Heartbeat
         */
        struct alignas(8) HeartbeatV1
        {
            uint64_t timestamp_ms; ///< 发送方时间戳（毫秒）
        };

        /**
         * @brief GoodbyeV1: 双向（断开连接）
         *
         * 控制平面消息：
         * - object_id = 0
         * - type_id = 0
         * - interface_id = HandshakeInterfaceId::Goodbye
         */
        struct alignas(8) GoodbyeV1
        {
            uint32_t reason; ///< 断开原因（GoodbyeReason 枚举）
            uint32_t reserved;
        };

        // ============================================================
        // 握手流程辅助函数
        // ============================================================

        /**
         * @brief 初始化 HelloRequestV1
         */
        inline void InitHelloRequest(
            HelloRequestV1& req,
            uint32_t        pid,
            const char*     plugin_name)
        {
            req.protocol_version = HelloRequestV1::CURRENT_PROTOCOL_VERSION;
            req.pid = pid;

            // 安全复制插件名称
            size_t name_len = 0;
            if (plugin_name != nullptr)
            {
                name_len = std::strlen(plugin_name);
                if (name_len >= HelloRequestV1::PLUGIN_NAME_SIZE)
                {
                    name_len = HelloRequestV1::PLUGIN_NAME_SIZE - 1;
                }
                std::memcpy(req.plugin_name, plugin_name, name_len);
            }
            req.plugin_name[name_len] = '\0';
        }

        /**
         * @brief 初始化 WelcomeResponseV1
         */
        inline void InitWelcomeResponse(
            WelcomeResponseV1& resp,
            uint16_t           session_id,
            uint32_t           status = WelcomeResponseV1::STATUS_SUCCESS)
        {
            resp.session_id = session_id;
            resp.reserved = 0;
            resp.status = status;
        }

        /**
         * @brief 初始化 ReadyRequestV1
         */
        inline void InitReadyRequest(ReadyRequestV1& req, uint16_t session_id)
        {
            req.session_id = session_id;
            req.reserved = 0;
        }

        /**
         * @brief 初始化 ReadyAckV1
         */
        inline void InitReadyAck(
            ReadyAckV1& ack,
            uint32_t    status = ReadyAckV1::STATUS_SUCCESS)
        {
            ack.status = status;
        }

        /**
         * @brief 初始化 HeartbeatV1
         */
        inline void InitHeartbeat(HeartbeatV1& hb, uint64_t timestamp_ms)
        {
            hb.timestamp_ms = timestamp_ms;
        }

        /**
         * @brief 初始化 GoodbyeV1
         */
        inline void InitGoodbye(
            GoodbyeV1&    gb,
            GoodbyeReason reason = GoodbyeReason::NormalShutdown)
        {
            gb.reason = static_cast<uint32_t>(reason);
            gb.reserved = 0;
        }

        // ============================================================
        // 握手状态机
        // ============================================================

        /**
         * @brief 握手状态
         */
        enum class HandshakeState : uint8_t
        {
            Disconnected = 0, ///< 未连接
            HelloSent = 1,    ///< HelloRequest 已发送
            WelcomeRecv = 2,  ///< WelcomeResponse 已接收
            ReadySent = 3,    ///< ReadyRequest 已发送
            Connected = 4,    ///< 握手完成，已连接
            Failed = 5        ///< 握手失败
        };

        /**
         * @brief 握手结果
         */
        struct HandshakeResult
        {
            HandshakeState state;
            uint16_t session_id; ///< 分配的 session_id（仅 Connected 状态有效）
            uint32_t error_code; ///< 错误码（仅 Failed 状态有效）
        };

    }
}
DAS_NS_END

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // DAS_CORE_IPC_HANDSHAKE_H
