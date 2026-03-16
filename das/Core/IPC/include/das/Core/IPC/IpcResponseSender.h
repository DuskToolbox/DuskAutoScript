#ifndef DAS_CORE_IPC_IPC_RESPONSE_SENDER_H
#define DAS_CORE_IPC_IPC_RESPONSE_SENDER_H

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
 * - IO 线程模式：使用 transport 直接发送（同步调用）
 * - 业务线程模式：通过 IpcRunLoop::PostSend 投递到 IO 线程发送
 */
class IpcResponseSender
{
public:
    /**
     * @brief 构造函数（IO 线程模式）
     * @param transport 异步 IPC 传输层（必须有效）
     */
    explicit IpcResponseSender(
        DefaultAsyncIpcTransport& DAS_LIFETIMEBOUND transport);

    /**
     * @brief 构造函数（业务线程模式）
     * @param run_loop IpcRunLoop 引用（用于 PostSend 投递）
     */
    explicit IpcResponseSender(IpcRunLoop& DAS_LIFETIMEBOUND run_loop);

    /**
     * @brief 发送响应（同步版本）
     *
     * IO 线程模式：使用 co_spawn + use_future 同步发送
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
     * @brief 获取 transport 指针（协程发送用）
     *
     * IAwaitableMessageHandler 使用此方法获取 transport，
     * 然后通过 co_await transport->SendCoroutine() 发送响应。
     *
     * @return transport 指针，如果为 IO 线程模式返回 nullptr
     */
    [[nodiscard]]
    DefaultAsyncIpcTransport* GetTransport() const
    {
        return transport_;
    }

private:
    DefaultAsyncIpcTransport* transport_ = nullptr; // IO 线程模式
    IpcRunLoop*               run_loop_ = nullptr;  // 业务线程模式
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RESPONSE_SENDER_H
