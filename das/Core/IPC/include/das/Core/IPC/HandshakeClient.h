#pragma once

#include <chrono>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/IDasBase.h>
#include <string>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief 握手客户端（使用 SendMessage）
 *
 * 使用 IpcRunLoop::SendMessage() 接口实现 IPC 握手流程：
 * 1. 发送 HELLO 并等待 WELCOME
 * 2. 发送 READY 并等待 READY_ACK
 */
class HandshakeClient
{
public:
    /**
     * @brief 构造函数
     * @param run_loop IPC 运行循环引用
     */
    explicit HandshakeClient(IpcRunLoop& run_loop);

    /**
     * @brief 发送 HELLO 并等待 WELCOME
     * @param client_name 客户端名称
     * @param out_session_id [out] 分配的 session_id
     * @param timeout 超时时间（默认 3 秒）
     * @return 操作结果
     */
    DasResult SendHelloAndWaitWelcome(
        const std::string&        client_name,
        uint16_t&                 out_session_id,
        std::chrono::milliseconds timeout = std::chrono::seconds(3));

    /**
     * @brief 发送 READY 并等待 READY_ACK
     * @param session_id 已分配的 session_id
     * @param timeout 超时时间（默认 3 秒）
     * @return 操作结果
     */
    DasResult SendReadyAndWaitAck(
        uint16_t                  session_id,
        std::chrono::milliseconds timeout = std::chrono::seconds(3));

    /**
     * @brief 完整握手流程
     *
     * 执行完整的握手流程：
     * 1. 发送 HELLO，等待 WELCOME
     * 2. 发送 READY，等待 READY_ACK
     *
     * @param client_name 客户端名称
     * @param out_session_id [out] 分配的 session_id
     * @param timeout 超时时间（默认 3 秒）
     * @return 操作结果
     */
    DasResult PerformHandshake(
        const std::string&        client_name,
        uint16_t&                 out_session_id,
        std::chrono::milliseconds timeout = std::chrono::seconds(3));

private:
    IpcRunLoop& run_loop_;
};

DAS_CORE_IPC_NS_END
