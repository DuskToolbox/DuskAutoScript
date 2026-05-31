#ifndef DAS_CORE_IPC_IPC_RESPONSE_SENDER_H
#define DAS_CORE_IPC_IPC_RESPONSE_SENDER_H

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <das/Core/IPC/IHostConnection.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/DasPtr.hpp>
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
 * 发送器只保存响应路由能力，不保存底层 transport 引用。
 */
class IpcResponseSender
{
public:
    struct SessionRoute
    {
        IpcRunLoop* run_loop = nullptr;
    };

    struct HostConnectionRoute
    {
        IpcRunLoop*             run_loop = nullptr;
        DasPtr<IHostConnection> connection;
    };

    /**
     * @brief 业务线程 / Host-local session 路由构造函数。
     */
    explicit IpcResponseSender(IpcRunLoop& run_loop DAS_LIFETIMEBOUND);

    explicit IpcResponseSender(SessionRoute route);

    explicit IpcResponseSender(HostConnectionRoute route);

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
    enum class RouteKind : uint8_t
    {
        None,
        Session,
        HostConnection
    };

    DasResult SendViaRoute(
        const ValidatedIPCMessageHeader& header,
        std::vector<uint8_t>&&           body);

    RouteKind               route_kind_ = RouteKind::None;
    IpcRunLoop*             run_loop_ = nullptr;
    DasPtr<IHostConnection> connection_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RESPONSE_SENDER_H
