#ifndef DAS_CORE_IPC_UNIX_ASYNC_IPC_TRANSPORT_H
#define DAS_CORE_IPC_UNIX_ASYNC_IPC_TRANSPORT_H

#ifndef _WIN32

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdint>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/DasExport.h>
#include <das/Utils/Expected.h>
#include <memory>
#include <string>
#include <vector>

#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/DasConfig.h>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief Unix/Linux/macOS 平台异步 IPC 传输层实现
 * @details
 * 使用 Unix Domain Socket 实现的异步 IPC 传输层。
 * 基于 boost::asio 协程设计。
 *
 * 架构：
 * - 服务端创建 Unix Domain Socket 文件并等待客户端连接
 * - 客户端连接到该 socket 文件
 * - 单 socket 双向通信
 * - 所有 I/O 通过 io_context 异步执行
 *
 * 使用示例：
 * @code
 * boost::asio::io_context io;
 *
 * // 异步创建
 * auto transport = co_await UnixAsyncIpcTransport::CreateAsync(io,
 * "/tmp/my_ipc", "", true);
 *
 * // 或延迟初始化
 * auto transport = UnixAsyncIpcTransport::CreateUninitialized(io);
 * co_await transport->InitializeAsync("/tmp/my_ipc", "", true);
 *
 * // 异步接收（协程）
 * auto result = co_await transport->ReceiveCoroutine();
 * @endcode
 */
class UnixAsyncIpcTransport
{
public:
    /// 工厂函数：异步创建并初始化 UnixAsyncIpcTransport 实例
    /// @param io_context boost::asio io_context 引用（生命周期绑定到返回值）
    /// @param read_endpoint 读取端点名称（Unix Domain Socket 文件路径）
    /// @param write_endpoint 写入端点名称（Unix 下忽略，使用单 socket）
    /// @param is_server 是否作为服务端
    /// @param max_message_size 最大消息大小（默认64KB）
    /// @return awaitable 包含 Expected<unique_ptr> 成功，Expected<error> 失败
    static boost::asio::awaitable<
        DAS::Utils::Expected<std::unique_ptr<UnixAsyncIpcTransport>>>
    CreateAsync(
        boost::asio::io_context& DAS_LIFETIMEBOUND io_context,
        const std::string&                         read_endpoint,
        const std::string&                         write_endpoint,
        bool                                       is_server,
        size_t                                     max_message_size = 65536);

    /// 工厂函数：创建未初始化的 UnixAsyncIpcTransport 实例
    /// @param io_context boost::asio io_context 引用（生命周期绑定到返回值）
    /// @return unique_ptr 需要后续调用 InitializeAsync() 完成初始化
    /// @note 用于需要延迟初始化的场景（如 Host 进程在 Run() 时异步连接）
    static std::unique_ptr<UnixAsyncIpcTransport> CreateUninitialized(
        boost::asio::io_context& DAS_LIFETIMEBOUND io_context);

    ~UnixAsyncIpcTransport();

    UnixAsyncIpcTransport(const UnixAsyncIpcTransport&) = delete;
    UnixAsyncIpcTransport& operator=(const UnixAsyncIpcTransport&) = delete;

    UnixAsyncIpcTransport(UnixAsyncIpcTransport&&) noexcept = default;
    UnixAsyncIpcTransport& operator=(UnixAsyncIpcTransport&&) noexcept =
        default;

    /// 初始化（同步版本）
    /// @deprecated 使用 CreateAsync() 或 InitializeAsync() 代替
    [[deprecated("Use CreateAsync() instead")]]
    DasResult Initialize(
        const std::string& read_endpoint,
        const std::string& write_endpoint,
        bool               is_server,
        size_t             max_message_size = 65536);

    /// 异步初始化（协程版本）
    boost::asio::awaitable<DasResult> InitializeAsync(
        const std::string& read_endpoint,
        const std::string& write_endpoint,
        bool               is_server,
        size_t             max_message_size = 65536);

    DasResult Connect(
        const std::string& read_endpoint,
        const std::string& write_endpoint);

    [[nodiscard]]
    bool IsConnected() const;

    [[nodiscard]]
    std::string GetEndpointName() const;

    void SetSharedMemoryPool(SharedMemoryPool* pool);

    /// 获取 io_context 引用
    /// @return io_context 引用，生命周期绑定到 this
    boost::asio::io_context& GetIoContext() DAS_LIFETIMEBOUND
    {
        return io_context_;
    }

    /// 异步接收协程
    [[nodiscard]]
    boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
    ReceiveCoroutine();

    /// 异步发送协程
    [[nodiscard]]
    boost::asio::awaitable<DasResult> SendCoroutine(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

private:
    // 私有构造函数（由工厂函数调用）
    explicit UnixAsyncIpcTransport(
        boost::asio::io_context& DAS_LIFETIMEBOUND io_context);

    // 私有清理函数 - 只能由析构函数调用
    void Uninitialize();

    // 异步方法
    boost::asio::awaitable<DasResult> CreateUnixSocketAsync(
        const std::string& socket_path);
    boost::asio::awaitable<DasResult> ConnectToUnixSocketAsync(
        const std::string& socket_path);

    // 服务端 acceptor（仅服务端使用）
    std::unique_ptr<boost::asio::local::stream_protocol::acceptor> acceptor_;

    boost::asio::io_context&                    io_context_;
    boost::asio::local::stream_protocol::socket socket_;

    std::string endpoint_name_;
    bool        is_server_ = false;
    bool        is_connected_ = false;
    size_t      max_message_size_ = 65536;

    SharedMemoryPool* shared_memory_pool_ = nullptr;

    std::vector<uint8_t> header_buffer_;
    std::vector<uint8_t> body_buffer_;
};

DAS_CORE_IPC_NS_END

#endif // !_WIN32

#endif // DAS_CORE_IPC_UNIX_ASYNC_IPC_TRANSPORT_H
