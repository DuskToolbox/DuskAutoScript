#pragma once

#include <chrono>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <stdexec/execution.hpp>
#include <string>

#include <das/Core/IPC/Config.h>

#include <chrono>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/IDasBase.h>
#include <stdexec/execution.hpp>
#include <string>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief 握手客户端（使用异步 SendMessageAsync）
 *
 * 使用 IpcRunLoop::SendMessageAsync() 接口实现 IPC 握手流程：
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

    // ===== 异步接口 =====

    /**
     * @brief 异步发送 HELLO 并等待 WELCOME（返回 sender）
     *
     * sender 完成时携带 pair<DasResult, vector<uint8_t>>，
     * 调用者需要自行解析 WelcomeResponseV1。
     *
     * @param client_name 客户端名称
     * @param timeout 超时时间
     * @return AwaitResponseSender
     */
    [[nodiscard]]
    AwaitResponseSender SendHelloAsync(
        const std::string&        client_name,
        std::chrono::milliseconds timeout = std::chrono::seconds(3));

    /**
     * @brief 异步发送 READY 并等待 READY_ACK（返回 sender）
     *
     * @param session_id 已分配的 session_id
     * @param timeout 超时时间
     * @return AwaitResponseSender
     */
    [[nodiscard]]
    AwaitResponseSender SendReadyAsync(
        uint16_t                  session_id,
        std::chrono::milliseconds timeout = std::chrono::seconds(3));

    /**
     * @brief 异步完整握手流程（返回 sender 管线）
     *
     * sender 完成时携带 pair<DasResult, uint16_t>:
     * - DasResult: 握手结果
     * - uint16_t: 分配的 session_id（成功时有效）
     *
     * 此 sender 可以被 RequestStop() 中断：
     *   - 如果 HELLO 阶段被中断，返回 DAS_E_IPC_TIMEOUT
     *   - 如果 READY 阶段被中断，返回 DAS_E_IPC_TIMEOUT
     *
     * @param client_name 客户端名称
     * @param timeout 超时时间
     * @return stdexec sender
     */
    [[nodiscard]]
    stdexec::sender auto PerformHandshakeAsync(
        const std::string&        client_name,
        std::chrono::milliseconds timeout = std::chrono::seconds(3));

private:
    IpcRunLoop& run_loop_;
};

// ============================================================================
// PerformHandshakeAsync 内联实现（因为返回 auto）
// ============================================================================

inline stdexec::sender auto HandshakeClient::PerformHandshakeAsync(
    const std::string&        client_name,
    std::chrono::milliseconds timeout)
{
    // sender 管线: HELLO → 解析 WELCOME → READY → 解析 ACK
    return SendHelloAsync(client_name, timeout)
           | stdexec::then(
               [this, timeout](
                   std::pair<DasResult, std::vector<uint8_t>> hello_result)
                   -> std::pair<DasResult, uint16_t>
               {
                   auto& [result, response] = hello_result;
                   if (DAS::IsFailed(result))
                   {
                       std::string msg = DAS_FMT_NS::format(
                           "SendHelloAsync failed: error = {}",
                           result);
                       DAS_LOG_ERROR(msg.c_str());
                       return {result, 0};
                   }

                   // 解析 WelcomeResponseV1
                   if (response.size() < sizeof(WelcomeResponseV1))
                   {
                       DAS_LOG_ERROR("Welcome response body too small");
                       return {DAS_E_IPC_INVALID_MESSAGE_BODY, 0};
                   }

                   const auto* welcome =
                       reinterpret_cast<const WelcomeResponseV1*>(
                           response.data());

                   if (welcome->status != WelcomeResponseV1::STATUS_SUCCESS)
                   {
                       std::string msg = DAS_FMT_NS::format(
                           "Welcome status error: {}",
                           welcome->status);
                       DAS_LOG_ERROR(msg.c_str());
                       return {DAS_E_IPC_HANDSHAKE_FAILED, 0};
                   }

                   if (welcome->session_id == 0)
                   {
                       DAS_LOG_ERROR("Received invalid session_id (0)");
                       return {DAS_E_IPC_HANDSHAKE_FAILED, 0};
                   }

                   // HELLO 成功，继续 READY
                   // 注意：这里使用 let_value 来组合异步操作
                   uint16_t session_id = welcome->session_id;

                   // 发送 READY 并等待 ACK（异步）
                   auto ready_sender = SendReadyAsync(session_id, timeout);
                   auto ready_result_opt =
                       stdexec::sync_wait(std::move(ready_sender));

                   if (!ready_result_opt.has_value())
                   {
                       DAS_LOG_ERROR(
                           "SendReadyAsync: sync_wait failed (cancelled?)");
                       return {DAS_E_IPC_TIMEOUT, 0};
                   }

                   auto& [ready_result, ready_response] =
                       std::get<0>(ready_result_opt.value());
                   if (DAS::IsFailed(ready_result))
                   {
                       std::string msg = DAS_FMT_NS::format(
                           "SendReadyAsync failed: error = {}",
                           ready_result);
                       DAS_LOG_ERROR(msg.c_str());
                       return {ready_result, 0};
                   }

                   // 解析 ReadyAckV1
                   if (ready_response.size() < sizeof(ReadyAckV1))
                   {
                       DAS_LOG_ERROR("ReadyAck response body too small");
                       return {DAS_E_IPC_INVALID_MESSAGE_BODY, 0};
                   }

                   const auto* ack = reinterpret_cast<const ReadyAckV1*>(
                       ready_response.data());

                   if (ack->status != ReadyAckV1::STATUS_SUCCESS)
                   {
                       std::string msg = DAS_FMT_NS::format(
                           "ReadyAck status error: {}",
                           ack->status);
                       DAS_LOG_ERROR(msg.c_str());
                       return {DAS_E_IPC_HANDSHAKE_FAILED, 0};
                   }

                   std::string info_msg = DAS_FMT_NS::format(
                       "PerformHandshakeAsync completed: session_id = {}",
                       session_id);
                   DAS_LOG_INFO(info_msg.c_str());

                   return {DAS_S_OK, session_id};
               });
}

DAS_CORE_IPC_NS_END
