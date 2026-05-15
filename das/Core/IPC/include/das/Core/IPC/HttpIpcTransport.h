#ifndef DAS_CORE_IPC_HTTP_IPC_TRANSPORT_H
#define DAS_CORE_IPC_HTTP_IPC_TRANSPORT_H

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <memory>
#include <string>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

class SharedMemoryPool;

/**
 * @brief 基于 WebSocket 的 IPC 传输实现
 * @details 使用 WebSocket 二进制帧替代 Named Pipe 进行 IPC 通信，
 *          实现与 Win32AsyncIpcTransport 相同的消息语义。
 *
 * 消息格式: 单个 WebSocket 二进制帧 = [IPCMessageHeader(32B)][body(body_size)]
 *
 * 与 Named Pipe 版本的区别:
 * - 无需 AsyncMutex（WebSocket 协议层保证帧有序）
 * - 无需 SharedMemoryPool（大消息直接作为完整帧发送）
 * - 无平台特定依赖（不依赖 Windows.h 或 unistd.h）
 */
class HttpIpcTransport
{
public:
    /**
     * @brief 从已建立的 TCP 连接构造 WebSocket 传输
     * @param socket 已连接的 TCP socket，所有权转移
     */
    explicit HttpIpcTransport(boost::asio::ip::tcp::socket&& socket);

    /**
     * @brief 从已升级的 WebSocket stream 构造传输
     * @param ws 已完成 WebSocket 升级的 stream，所有权转移
     *
     * 用于 Server 端：HttpIpcServer 完成 WebSocket accept 后，
     * 将已升级的 stream 直接移交。
     */
    explicit HttpIpcTransport(
        boost::beast::websocket::stream<boost::asio::ip::tcp::socket>&& ws);

    ~HttpIpcTransport();

    HttpIpcTransport(const HttpIpcTransport&) = delete;
    HttpIpcTransport& operator=(const HttpIpcTransport&) = delete;

    HttpIpcTransport(HttpIpcTransport&&) noexcept;
    HttpIpcTransport& operator=(HttpIpcTransport&&) noexcept;

    /**
     * @brief 异步发送 IPC 消息
     * @param header 已验证的消息头（32 字节）
     * @param body 消息体数据指针
     * @param body_size 消息体长度
     * @return awaitable<DasResult> DAS_S_OK 成功，DAS_E_IPC_SEND_FAILED 失败
     */
    [[nodiscard]]
    boost::asio::awaitable<DasResult> SendCoroutine(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

    /**
     * @brief 异步接收 IPC 消息
     * @return awaitable<variant<DasResult, AsyncIpcMessage>>
     *         成功返回 AsyncIpcMessage，失败返回 DasResult 错误码
     */
    [[nodiscard]]
    boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
    ReceiveCoroutine();

    /**
     * @brief 检查连接是否仍然活跃
     */
    [[nodiscard]]
    bool IsConnected() const;

    /**
     * @brief 获取关联的 io_context
     */
    boost::asio::io_context& GetIoContext();

    /**
     * @brief 设置共享内存池（HTTP 模式下为 no-op）
     * @note HTTP 传输不使用共享内存，大消息直接通过 WebSocket 帧传输
     *
     * TODO(RAII): SetSharedMemoryPool 是两阶段初始化的反模式。
     * SharedMemoryPool 应通过构造函数注入。当前保留此方法是因为
     * AnyTransport 包装器需要统一的 SetXxx 接口。
     * 修复需要重构整个传输层接口层次。
     */
    void SetSharedMemoryPool(SharedMemoryPool* pool);

    /**
     * @brief 获取远端端点名称
     */
    [[nodiscard]]
    std::string GetEndpointName() const;

    /**
     * @brief 关闭连接并释放资源
     */
    void Cleanup();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_HTTP_IPC_TRANSPORT_H
