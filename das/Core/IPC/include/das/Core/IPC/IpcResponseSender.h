#ifndef DAS_CORE_IPC_IPC_RESPONSE_SENDER_H
#define DAS_CORE_IPC_IPC_RESPONSE_SENDER_H

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <optional>
#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <vector>

#include <das/Core/IPC/Config.h>
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
     * @brief 构造函数（IO 线程模式，仅 transport）
     * @param transport 异步 IPC 传输层（必须有效）
     *
     * 此模式下 SendResponse 会失败（无 run_loop），
     * SendResponseAsync 直接通过 transport 发送（绕过队列）。
     * 仅用于无法获取 IpcRunLoop 的场景。
     */
    explicit IpcResponseSender(AnyTransport& transport DAS_LIFETIMEBOUND);

    /**
     * @brief 构造函数（业务线程模式，仅 run_loop）
     * @param run_loop IpcRunLoop 引用（用于 PostSend 投递）
     *
     * 此模式下 SendResponse 通过 PostSend 投递到 IO 线程队列。
     * SendResponseAsync 通过 PostSendWithTransport 投递（需要 transport
     * 也被设置）。
     */
    explicit IpcResponseSender(IpcRunLoop& run_loop DAS_LIFETIMEBOUND);

    /**
     * @brief 构造函数（双模式，IO 线程协程中使用）
     * @param transport 异步 IPC 传输层（必须有效）
     * @param run_loop IpcRunLoop 引用（用于发送队列序列化）
     *
     * 推荐在 IO 线程协程中使用此构造函数。
     * SendResponse 和 SendResponseAsync 都通过发送队列序列化，
     * 防止多个 async_write 并发写入同一管道。
     */
    IpcResponseSender(
        AnyTransport& transport DAS_LIFETIMEBOUND,
        IpcRunLoop&   run_loop  DAS_LIFETIMEBOUND);

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
     * @brief 同步发送响应（move 语义版本）
     *
     * 直接接管 body 的所有权，避免不必要的拷贝。
     *
     * @param header 响应消息头
     * @param body 响应消息体（右值引用，会被 move）
     * @return DasResult 发送结果
     */
    DasResult SendResponse(
        const ValidatedIPCMessageHeader& header,
        std::vector<uint8_t>&&           body);

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

    /**
     * @brief 异步发送响应（move 语义版本）
     *
     * 直接接管 body 的所有权，避免不必要的拷贝。
     *
     * @param header 响应消息头
     * @param body 响应消息体（右值引用，会被 move）
     * @return boost::asio::awaitable<DasResult> 发送结果
     */
    boost::asio::awaitable<DasResult> SendResponseAsync(
        const ValidatedIPCMessageHeader& header,
        std::vector<uint8_t>&&           body);

private:
    std::optional<AnyTransportRef> transport_; // IO 线程模式
    IpcRunLoop*                    run_loop_ = nullptr; // 业务线程模式
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RESPONSE_SENDER_H
