#ifndef DAS_CORE_IPC_UNIX_ASYNC_IPC_TRANSPORT_H
#define DAS_CORE_IPC_UNIX_ASYNC_IPC_TRANSPORT_H

#ifndef _WIN32

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <cstdint>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Utils/Expected.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/DasExport.h>
#include <memory>
#include <string>
#include <vector>

#include <das/Core/IPC/Config.h>
#include <das/DasConfig.h>
#include <das/Core/IPC/IpcErrors.h>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief Unix/Linux/macOS 平台异步 IPC 传输层实现
 * @details
 * 使用 Unix Domain Socket 实现的异步 IPC 传输层。
 * 基于 boost::asio 协程设计。
 *
 * 架构：
 * - 服务端创建 Unix Domain Socket 文件
 * - 客户端连接到该文件
 * - 所有 I/O 通过 io_context 异步执行
 *
 * 使用示例：
 * @code
 * boost::asio::io_context io;
 * UnixAsyncIpcTransport transport(io);
 *
 * // 服务端
 * transport.Initialize("/tmp/my_ipc", true);
 *
 * // 客户端
 * transport.Connect("/tmp/my_ipc");
 *
 * // 异步接收（协程）
 * auto result = co_await transport.ReceiveCoroutine();
 * @endcode
 */
class UnixAsyncIpcTransport
{
public:
    /// 工厂函数：创建并初始化 UnixAsyncIpcTransport 实例
    /// @param io_context boost::asio io_context 引用（生命周期绑定到返回值）
    /// @param read_endpoint 读取端点名称
    /// @param write_endpoint 写入端点名称
    /// @param is_server 是否作为服务端
    /// @param max_message_size 最大消息大小（默认64KB）
    /// @return Expected 包含 unique_ptr 成功，错误码失败
    static DAS::Utils::Expected<std::unique_ptr<UnixAsyncIpcTransport>> Create(
        boost::asio::io_context& DAS_LIFETIMEBOUND io_context,
        const std::string&       read_endpoint,
        const std::string&       write_endpoint,
        bool                     is_server,
        size_t                   max_message_size = 65536);

    /**
     * @brief 析构函数
     * @note 自动调用 Close()
     */
    ~UnixAsyncIpcTransport();

    // === 禁止拷贝 ===
    UnixAsyncIpcTransport(const UnixAsyncIpcTransport&) = delete;
    UnixAsyncIpcTransport& operator=(const UnixAsyncIpcTransport&) = delete;

    // === 允许移动 ===
    UnixAsyncIpcTransport(UnixAsyncIpcTransport&&) noexcept = default;
    UnixAsyncIpcTransport& operator=(UnixAsyncIpcTransport&&) noexcept =
        default;

    // === 生命周期管理（同步） ===

    /**
     * @brief 初始化传输层（服务端）
     * @deprecated 使用 Create() 工厂函数代替
     * @param endpoint_name 端点名称（Unix Domain Socket 文件路径）
     * @param is_server 是否为服务端模式
     * @param max_message_size 最大消息大小（默认 64KB）
     * @return DAS_S_OK 成功
     * @return DAS_E_IPC_MESSAGE_QUEUE_FAILED 创建失败
     */
    [[deprecated("Use Create() instead")]]
    DasResult Initialize(
        const std::string& read_endpoint,
        const std::string& write_endpoint,
        bool               is_server,
        size_t             max_message_size = 65536);

    DasResult Connect(
        const std::string& read_endpoint,
        const std::string& write_endpoint);

    /**
     * @brief 关闭传输层
     * @note 幂等操作，可以多次调用
     */
    void Close();

    // === 查询接口 ===

    /**
     * @brief 检查是否已连接
     */
    [[nodiscard]]
    bool IsConnected() const;

    /**
     * @brief 获取端点名称
     */
    [[nodiscard]]
    std::string GetEndpointName() const;

    /**
     * @brief 设置共享内存池（用于大消息传输）
     * @param pool 共享内存池指针（调用者负责生命周期管理）
     * @note 必须在 Initialize() 或 Connect() 之前调用
     */
    void SetSharedMemoryPool(SharedMemoryPool* DAS_LIFETIMEBOUND pool);

    /// 获取 io_context 引用
    /// @return io_context 引用，生命周期绑定到 this
    boost::asio::io_context& GetIoContext() DAS_LIFETIMEBOUND { return impl_->io_context; }

    // === 协程接口（用于事件驱动模式） ===

    /**
     * @brief 异步接收协程
     * @return boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
     */
    boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
    ReceiveCoroutine();

    /**
     * @brief 异步发送协程
     * @return boost::asio::awaitable<DasResult>
     */
    boost::asio::awaitable<DasResult> SendCoroutine(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

private:
    // 私有构造函数（由 Create() 工厂函数调用）
    explicit UnixAsyncIpcTransport(boost::asio::io_context& DAS_LIFETIMEBOUND io_context);

    /**
     * @brief 创建 Unix Domain Socket（服务端）
     * @param socket_path Socket 文件路径
     * @return DAS_S_OK 成功
     * @return DAS_E_IPC_MESSAGE_QUEUE_FAILED 创建失败
     */
    DasResult CreateUnixSocket(const std::string& socket_path);

    /**
     * @brief 连接到 Unix Domain Socket（客户端）
     * @param socket_path Socket 文件路径
     * @return DAS_S_OK 成功
     * @return DAS_E_IPC_MESSAGE_QUEUE_FAILED 连接失败
     */
    DasResult ConnectToUnixSocket(const std::string& socket_path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

DAS_CORE_IPC_NS_END

#endif // !_WIN32

#endif // DAS_CORE_IPC_UNIX_ASYNC_IPC_TRANSPORT_H
