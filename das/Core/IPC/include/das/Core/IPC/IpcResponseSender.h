#ifndef DAS_CORE_IPC_IPC_RESPONSE_SENDER_H
#define DAS_CORE_IPC_IPC_RESPONSE_SENDER_H

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <vector>

#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DefaultAsyncIpcTransport.h>
#include <das/DasConfig.h>

DAS_CORE_IPC_NS_BEGIN

// 前向声明
class IpcRunLoop;

/**
 * @brief IPC 响应发送器
 *
 * 提供响应发送功能的轻量级包装器。
 * IMessageHandler 通过此接口发送响应。
 *
 * 支持两种模式：
 * - IO 线程模式（transport 构造）：协程 handler 通过 SendResponseAsync 发送
 * - 业务线程模式（run_loop 构造）：同步 handler 通过 SendResponse 发送
 */
class IpcResponseSender
{
public:
    /**
     * @brief 构造函数（IO 线程模式）
     * @param transport 异步 IPC 传输层（必须有效）
     */
    explicit IpcResponseSender(
        DefaultAsyncIpcTransport& transport DAS_LIFETIMEBOUND);

    /**
     * @brief 构造函数（业务线程模式）
     * @param run_loop IpcRunLoop 引用（用于 PostSend 投递）
     */
    explicit IpcResponseSender(IpcRunLoop& run_loop DAS_LIFETIMEBOUND);

    /**
     * @brief 同步发送响应（业务线程版本）
     *
     * 业务线程模式：调用 IpcRunLoop::PostSend 投递到 IO 线程
     *
     * @param header 响应消息头
     * @param body 响应消息体
     * @return DasResult 发送结果
     */
    DasResult SendResponse(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body);

    /**
     * @brief 异步发送响应（协程版本，用于 IO 线程上下文）
     *
     * IO 线程中的协程 handler 使用此方法发送响应，
     * 内部直接 co_await transport->SendCoroutine()。
     *
     * @param header 响应消息头
     * @param body 响应消息体
     * @return boost::asio::awaitable<DasResult> 发送结果
     */
    boost::asio::awaitable<DasResult> SendResponseAsync(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body);

private:
    DefaultAsyncIpcTransport* transport_ = nullptr; // IO 线程模式
    IpcRunLoop*               run_loop_ = nullptr;  // 业务线程模式
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RESPONSE_SENDER_H
