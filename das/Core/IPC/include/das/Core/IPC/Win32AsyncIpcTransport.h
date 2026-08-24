#ifndef DAS_CORE_IPC_WIN32_ASYNC_IPC_TRANSPORT_H
#define DAS_CORE_IPC_WIN32_ASYNC_IPC_TRANSPORT_H

#ifdef DAS_WINDOWS

#include <array>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/windows/object_handle.hpp>
#include <boost/asio/windows/stream_handle.hpp>
#include <cstdint>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/AsyncMutex.h>
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
    static boost::asio::awaitable<
        DAS::Utils::Expected<std::unique_ptr<Win32AsyncIpcTransport>>>
    CreateAsync(
        boost::asio::io_context& io_context DAS_LIFETIMEBOUND,
        const std::string&                  read_endpoint,
        const std::string&                  write_endpoint,
        bool                                is_server,
        size_t                              max_message_size = 65536);

    /// 工厂函数：创建未初始化的 Win32AsyncIpcTransport 实例
    /// @param io_context boost::asio io_context 引用（生命周期绑定到返回值）
    /// @return unique_ptr 需要后续调用 InitializeAsync() 完成初始化
    /// @note 用于需要延迟初始化的场景（如 Host 进程在 Run() 时异步连接）
    static std::unique_ptr<Win32AsyncIpcTransport> CreateUninitialized(
        boost::asio::io_context& io_context DAS_LIFETIMEBOUND);

    ~Win32AsyncIpcTransport();

    Win32AsyncIpcTransport(const Win32AsyncIpcTransport&) = delete;
    Win32AsyncIpcTransport& operator=(const Win32AsyncIpcTransport&) = delete;

    Win32AsyncIpcTransport(Win32AsyncIpcTransport&&) noexcept;
    Win32AsyncIpcTransport& operator=(Win32AsyncIpcTransport&&) noexcept;

    /// 异步初始化（协程版本）
    boost::asio::awaitable<DasResult> InitializeAsync(
        const std::string& read_endpoint,
        const std::string& write_endpoint,
        bool               is_server,
        size_t             max_message_size = 65536,
        SharedMemoryPool*  pool = nullptr);

    [[nodiscard]]
    bool IsConnected() const;

    [[nodiscard]]
    std::string GetEndpointName() const;

    void SetSharedMemoryPool(SharedMemoryPool* pool);

    /// 获取 io_context 引用
    /// @return io_context 引用，生命周期绑定到 this
    boost::asio::io_context& GetIoContext() DAS_LIFETIMEBOUND;

    /// 直接返回协程接口（用于 IpcRunLoop 的事件驱动模式）
    [[nodiscard]]
    boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
    ReceiveCoroutine();

    [[nodiscard]]
    boost::asio::awaitable<DasResult> SendCoroutine(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

    /// scatter-gather 发送协程：header 与 N 段构造 const_buffer 序列一次
    /// async_write（帧数不变写次数归一——pipe 字节流写次数对接收端不可见）
    /// @param header 已验证的消息头（32 字节；与段所指备存储同须存活至
    ///               本协程完成）
    /// @param segments 非拥有段序列（rvalue——段列表 move 进协程帧持有；
    ///                 契约见 AsyncIpcTransport.h 的 SendSegment 定义）
    /// @note 仅承接直写路径的小帧拼装：总长（header + 段总和）达到
    ///       LARGE_MESSAGE_THRESHOLD 即拒发（SHM 大消息路径维持既有单段
    ///       语义不接纳 scatter，且无降级直写）
    [[nodiscard]]
    boost::asio::awaitable<DasResult> SendScatter(
        const ValidatedIPCMessageHeader& header,
        std::vector<SendSegment>&&       segments);

private:
    struct State;

    // 私有构造函数（由 Create() 工厂函数调用）
    explicit Win32AsyncIpcTransport(
        boost::asio::io_context& io_context DAS_LIFETIMEBOUND);

    // 私有清理函数 - 只能由析构函数调用
    void Cleanup();

    DasResult CreateNamedPipe(const std::string& pipe_name, bool is_read_pipe);
    DasResult ConnectToNamedPipe(
        const std::string& pipe_name,
        bool               is_read_pipe);

    // 异步方法
    boost::asio::awaitable<std::variant<DasResult, HANDLE>> OpenPipeAsync(
        const std::string& full_name,
        bool               is_read_pipe);

    boost::asio::awaitable<DasResult> WaitForClientConnectionAsync(
        HANDLE h_pipe);

    boost::asio::awaitable<std::variant<DasResult, HANDLE>>
    CreateNamedPipeAsync(
        const std::string& pipe_name,
        bool               is_read_pipe,
        bool               wait_for_connection);

    std::shared_ptr<State> state_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_WINDOWS

#endif // DAS_CORE_IPC_WIN32_ASYNC_IPC_TRANSPORT_H
