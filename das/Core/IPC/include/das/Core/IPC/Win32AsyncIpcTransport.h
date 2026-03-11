#ifndef DAS_CORE_IPC_WIN32_ASYNC_IPC_TRANSPORT_H
#define DAS_CORE_IPC_WIN32_ASYNC_IPC_TRANSPORT_H

#ifdef _WIN32

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/windows/object_handle.hpp>
#include <boost/asio/windows/stream_handle.hpp>
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
#include <optional>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

class Win32AsyncIpcTransport
{
public:
    /// 工厂函数：异步创建并初始化 Win32AsyncIpcTransport 实例
    /// @param io_context boost::asio io_context 引用（生命周期绑定到返回值）
    /// @param read_endpoint 读取端点名称
    /// @param write_endpoint 写入端点名称
    /// @param is_server 是否作为服务端
    /// @param max_message_size 最大消息大小（默认64KB）
    /// @return awaitable 包含 Expected<unique_ptr> 成功，Expected<error> 失败
    static boost::asio::awaitable<DAS::Utils::Expected<std::unique_ptr<Win32AsyncIpcTransport>>>
    CreateAsync(
        boost::asio::io_context& DAS_LIFETIMEBOUND io_context,
        const std::string&       read_endpoint,
        const std::string&       write_endpoint,
        bool                     is_server,
        size_t                   max_message_size = 65536);

    /// 工厂函数：创建未初始化的 Win32AsyncIpcTransport 实例
    /// @param io_context boost::asio io_context 引用（生命周期绑定到返回值）
    /// @return unique_ptr 需要后续调用 InitializeAsync() 完成初始化
    /// @note 用于需要延迟初始化的场景（如 Host 进程在 Run() 时异步连接）
    static std::unique_ptr<Win32AsyncIpcTransport> CreateUninitialized(
        boost::asio::io_context& DAS_LIFETIMEBOUND io_context);

    ~Win32AsyncIpcTransport();

    Win32AsyncIpcTransport(const Win32AsyncIpcTransport&) = delete;
    Win32AsyncIpcTransport& operator=(const Win32AsyncIpcTransport&) = delete;

    Win32AsyncIpcTransport(Win32AsyncIpcTransport&&) noexcept = default;
    Win32AsyncIpcTransport& operator=(Win32AsyncIpcTransport&&) noexcept =
        default;

    /// 初始化（服务端）
    /// @deprecated 使用 Create() 工厂函数代替
    [[deprecated("Use Create() instead")]]
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

    void Close();

    [[nodiscard]]
    bool IsConnected() const;

    [[nodiscard]]
    std::string GetEndpointName() const;

    void SetSharedMemoryPool(SharedMemoryPool* pool);

    /// 获取 io_context 引用
    /// @return io_context 引用，生命周期绑定到 this
    boost::asio::io_context& GetIoContext() DAS_LIFETIMEBOUND { return io_context_; }

    /// 直接返回协程接口（用于 IpcRunLoop 的事件驱动模式）
    [[nodiscard]]
    boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
    ReceiveCoroutine();

    [[nodiscard]]
    boost::asio::awaitable<DasResult> SendCoroutine(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

private:
    // 私有构造函数（由 Create() 工厂函数调用）
    explicit Win32AsyncIpcTransport(boost::asio::io_context& DAS_LIFETIMEBOUND io_context);

    DasResult CreateNamedPipe(const std::string& pipe_name, bool is_read_pipe);
    DasResult ConnectToNamedPipe(const std::string& pipe_name, bool is_read_pipe);

    // 异步方法
    boost::asio::awaitable<std::variant<DasResult, HANDLE>> OpenPipeAsync(
        const std::string& full_name,
        bool               is_read_pipe);

    boost::asio::awaitable<DasResult> WaitForClientConnectionAsync(HANDLE h_pipe);

    boost::asio::awaitable<std::variant<DasResult, HANDLE>> CreateNamedPipeAsync(
        const std::string& pipe_name,
        bool               is_read_pipe,
        bool               wait_for_connection);

    boost::asio::io_context&            io_context_;
    boost::asio::windows::stream_handle read_pipe_;
    boost::asio::windows::stream_handle write_pipe_;

    std::string endpoint_name_;
    bool        is_server_ = false;
    bool        is_connected_ = false;
    size_t      max_message_size_ = 65536;

    SharedMemoryPool* shared_memory_pool_ = nullptr;
};

DAS_CORE_IPC_NS_END

#endif // _WIN32

#endif // DAS_CORE_IPC_WIN32_ASYNC_IPC_TRANSPORT_H
