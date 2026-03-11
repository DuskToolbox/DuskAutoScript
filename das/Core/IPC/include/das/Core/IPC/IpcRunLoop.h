#ifndef DAS_CORE_IPC_IPC_RUN_LOOP_H
#define DAS_CORE_IPC_IPC_RUN_LOOP_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/IDasBase.h>
#include <das/Utils/Expected.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexec/execution.hpp>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/execution.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/DefaultAsyncIpcTransport.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>

#include <memory>

DAS_CORE_IPC_NS_BEGIN

class IMessageHandler;
class ConnectionManager;
class HostLauncher;
class IpcRunLoop; // Forward declaration for templates

namespace Host
{
    class HandshakeHandler;
}

DAS_CORE_IPC_NS_END

DAS_CORE_IPC_NS_BEGIN

/// 异步 pending call 的完成回调类型
using PendingCallCompletion =
    std::function<void(DasResult, std::vector<uint8_t>)>;

/// 异步 pending call 状态（IOCP 风格）
struct PendingCallState
{
    uint64_t                              call_id;
    std::vector<uint8_t>                  response_buffer;
    std::chrono::steady_clock::time_point deadline;
    PendingCallCompletion                 on_complete;
};

//=============================================================================
// AwaitResponseSender — 真正的异步 IPC 响应等待 sender
//=============================================================================

/**
 * @brief AwaitResponseSender 的 OperationState
 *
 * start() 时注册 on_complete 到 PendingCallState，
 * 由事件循环驱动完成（零轮询）。
 */
template <class Receiver>
struct AwaitResponseOperation
{
    IpcRunLoop*               loop_;
    uint64_t                  call_id_;
    std::chrono::milliseconds timeout_;
    Receiver                  rcvr_;

    friend void tag_invoke(
        stdexec::start_t,
        AwaitResponseOperation& self) noexcept
    {
        // 错误路径：loop_ 为 nullptr 表示 PrepareSendRequest 已失败
        if (!self.loop_)
        {
            auto error_code = static_cast<DasResult>(self.call_id_);
            stdexec::set_value(
                std::move(self.rcvr_),
                std::make_pair(error_code, std::vector<uint8_t>{}));
            return;
        }

        auto deadline = std::chrono::steady_clock::now() + self.timeout_;

        // 注册完成回调到 PendingCallState
        self.loop_->RegisterPendingCompletion(
            self.call_id_,
            deadline,
            [rcvr = std::move(self.rcvr_)](
                DasResult            result,
                std::vector<uint8_t> response) mutable
            {
                stdexec::set_value(
                    std::move(rcvr),
                    std::make_pair(result, std::move(response)));
            });
    }
};

/**
 * @brief 异步等待 IPC 响应的 sender
 *
 * 完成时携带 pair<DasResult, vector<uint8_t>>。
 * 不阻塞调用线程，由消息循环事件驱动完成。
 */
struct AwaitResponseSender
{
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(
            std::pair<DasResult, std::vector<uint8_t>>)>;

    IpcRunLoop*               loop_;
    uint64_t                  call_id_;
    std::chrono::milliseconds timeout_;

    template <class Receiver>
    friend auto tag_invoke(
        stdexec::connect_t,
        AwaitResponseSender self,
        Receiver            rcvr) noexcept -> AwaitResponseOperation<Receiver>
    {
        return {self.loop_, self.call_id_, self.timeout_, std::move(rcvr)};
    }
};

//=============================================================================
// IpcRunLoop — 异步 IPC 运行时
//=============================================================================

class IpcRunLoop
{
public:
    /// 工厂函数：创建 IpcRunLoop 实例
    /// @return Expected 包含 unique_ptr 成功，错误码失败
    static DAS::Utils::Expected<std::unique_ptr<IpcRunLoop>> Create();

    /// 工厂函数：创建 IpcRunLoop 实例（用于 Host 进程）
    /// @param read_queue_name 读取队列名称
    /// @param write_queue_name 写入队列名称
    /// @param is_server 是否作为服务端（创建队列）
    /// @return Expected 包含 unique_ptr 成功，错误码失败
    /// @deprecated Host 模式应由 IpcContext 持有 transport
    [[deprecated("Use Create() and manage transport externally")]]
    static DAS::Utils::Expected<std::unique_ptr<IpcRunLoop>> CreateForHost(
        const std::string& read_queue_name,
        const std::string& write_queue_name,
        bool               is_server);

    ~IpcRunLoop();

    // 阻塞式消息循环
    DasResult Run();

    /**
     * @brief 仅设置 running_ 标志为 false，不 join 线程
     *
     * 用于在事件循环线程内部（如 GOODBYE 回调中）安全请求退出，
     * 避免在事件循环线程上调用 join 导致死锁。
     * 线程的 join由调用方完成。
     */
    void RequestStop();

    /**
     * @brief 注册消息处理器
     * @param handler 处理器实例（所有权转移）
     */
    void RegisterHandler(std::unique_ptr<IMessageHandler> handler);

    /**
     * @brief 注册控制平面消息处理器
     *
     * 控制平面消息使用单独的处理器处理。
     * 所有控制平面消息都会被路由到这个处理器。
     *
     * @param handler 控制平面处理器实例（所有权转移）
     */
    void RegisterControlPlaneHandler(std::unique_ptr<IMessageHandler> handler);

    /**
     * @brief 按接口 ID 查找处理器
     * @param interface_id 接口 ID
     * @return 处理器指针，未找到返回 nullptr
     */
    [[nodiscard]]
    IMessageHandler* GetHandler(uint32_t interface_id) const;

    /**
     * @brief 异步 IPC 调用（返回 sender）
     *
     * 返回 sender，完成时携带 pair<DasResult, vector<uint8_t>>。
     * 真正的异步实现：注册到消息循环，由事件驱动完成（零轮询）。
     *
     * @param request_header 请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @param timeout 超时时间（默认30秒）
     * @return stdexec::sender 包含 pair<DasResult, vector<uint8_t>>
     */
    [[nodiscard]]
    stdexec::sender auto SendMessageAsync(
        const ValidatedIPCMessageHeader& request_header,
        const uint8_t*                   body,
        size_t                           body_size,
        std::chrono::milliseconds        timeout = std::chrono::seconds(30));

    /**
     * @brief 使用指定 transport 的异步 IPC 调用
     *
     * 用于转发消息到其他进程时，使用外部 transport 发送。
     *
     * @param transport 目标传输层（生命周期绑定到返回值）
     * @param request_header 请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @param timeout 超时时间（默认30秒）
     * @return stdexec::sender 包含 pair<DasResult, vector<uint8_t>>
     */
    [[nodiscard]]
    stdexec::sender auto SendMessageAsync(
        DefaultAsyncIpcTransport* DAS_LIFETIMEBOUND transport,
        const ValidatedIPCMessageHeader&            request_header,
        const uint8_t*                              body,
        size_t                                      body_size,
        std::chrono::milliseconds timeout = std::chrono::seconds(30));

    bool IsRunning() const;

    /// 获取 io_context 引用（用于 HostLauncher 等需要共享 io_context 的场景）
    /// 注意：仅在 Initialize() 成功后可用
    /// @return io_context 引用，生命周期绑定到 this
    boost::asio::io_context& GetIoContext() DAS_LIFETIMEBOUND
    {
        return *io_context_;
    }

    /**
     * @brief 获取 ConnectionManager（MainProcess 模式）
     *
     * ConnectionManager 用于管理 HostLauncher 实例。
     * 在 Host 模式下返回 nullptr。
     *
     * @return ConnectionManager* 指针，生命周期绑定到 this，未初始化返回
     * nullptr
     */
    ConnectionManager* GetConnectionManager() DAS_LIFETIMEBOUND
    {
        return connection_manager_.get();
    }

    /**
     * @brief 注册 HostLauncher 并启动接收循环
     *
     * 在 HostLauncher::Start() 成功后调用。
     * Transport 将在注册后立即开始接收消息。
     *
     * @param launcher HostLauncher 实例（DasPtr 所有权转移）
     * @return DasResult DAS_S_OK 成功
     */
    DasResult RegisterHostLauncher(DasPtr<IHostLauncher> launcher);

    friend class ::Das::Core::IPC::Host::HandshakeHandler;

    // AwaitResponseOperation 需要访问内部方法
    template <class Receiver>
    friend struct AwaitResponseOperation;

    /**
     * @brief 分发消息到注册的处理器（协程版本）
     *
     * 使用 co_await 调用 handler->HandleMessage()，
     * 通过 transport 发送响应。
     *
     * @param header 消息头
     * @param body 消息体
     * @param transport 用于发送响应的 transport
     * @return boost::asio::awaitable<void>
     */
    boost::asio::awaitable<void> DispatchToHandlerCoroutine(
        const IPCMessageHeader&     header,
        const std::vector<uint8_t>& body,
        DefaultAsyncIpcTransport&   transport);

    //=========================================================================
    // 事件驱动方法（内部使用）
    //=========================================================================

    /**
     * @brief 为指定 Transport 启动异步接收循环
     *
     * 在 RegisterHostLauncher 后调用，为该 Transport 启动接收协程。
     * 协程会捕获 launcher 的 DasPtr 以保持 transport 存活。
     *
     * @param session_id 会话 ID
     * @param launcher HostLauncher 的 DasPtr
     */
    void StartAsyncReceiveForTransport(
        uint16_t              session_id,
        DasPtr<IHostLauncher> launcher);

    /**
     * @brief 调度超时检查定时器
     *
     * 寏隔一段时间检查 pending senders 是否超时。
     */
    void ScheduleTimeoutCheck();

    /**
     * @brief 使用指定 transport 准备发送请求
     *
     * @param transport 目标传输层
     * @param request_header 原始请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @return pair<DasResult, uint64_t>: 结果码和分配的 call_id
     */
    std::pair<DasResult, uint64_t> PrepareSendRequestWithTransport(
        DefaultAsyncIpcTransport*        transport,
        const ValidatedIPCMessageHeader& request_header,
        const uint8_t*                   body,
        size_t                           body_size);

    /**
     * @brief 完成指定 call_id 的 pending call（IOCP 风格）
     *
     * 从 pending_calls_ 中取出 on_complete 回调并调用，
     * 在 mutex 外执行回调以避免死锁。
     *
     * @param call_id 调用 ID
     * @param result 结果码
     * @param response 响应体
     */
    void CompletePendingCall(
        uint64_t             call_id,
        DasResult            result,
        std::vector<uint8_t> response);

    /**
     * @brief 扫描并完成所有超时的 pending call
     *
     * 由 ScheduleTimeoutCheck 定时调用。
     * 检查所有 pending call 的 deadline，超时的调用
     * on_complete(DAS_E_IPC_TIMEOUT, {})。
     */
    void TickPendingSenders();

    /**
     * @brief 获取最近的 pending call 超时时间（毫秒）
     *
     * 用于 ScheduleTimeoutCheck 计算 timed_receive 的超时参数。
     * 如果没有 pending call，返回 0（无限等待）。
     *
     * @return 距离最近 deadline 的毫秒数，最小为 1
     */
    uint32_t GetNearestDeadlineMs() const;

    /**
     * @brief 注册 pending call 的完成回调（内部使用）
     *
     * 由 AwaitResponseOperation::start() 调用，将 on_complete
     * 回调和 deadline 设置到 PendingCallState 中。
     *
     * @param call_id 调用 ID
     * @param deadline 超时截止时间
     * @param on_complete 完成回调
     */
    void RegisterPendingCompletion(
        uint64_t                              call_id,
        std::chrono::steady_clock::time_point deadline,
        PendingCallCompletion                 on_complete);

    //=========================================================================
    // 成员变量
    //=========================================================================

    /// pending calls 映射
    std::unordered_map<uint64_t, PendingCallState> pending_calls_;

    // async_transport_ 已移除
    // IpcRunLoop 不再持有任何 transport，所有 transport 由外部管理
    // - MainProcess 模式：HostLauncher 持有 transport
    // - Host 模式：IpcContext 持有 transport

    /// io_context 用于驱动异步 I/O
    std::unique_ptr<boost::asio::io_context> io_context_;

    /// 超时计时器（用于 pending call 超时管理）
    std::unique_ptr<boost::asio::steady_timer> timeout_timer_;

    /// work guard 保持 io_context 运行（确保 Run() 阻塞直到 RequestStop()）
    using WorkGuard = boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>;
    std::unique_ptr<WorkGuard> work_guard_;

    /// 消息处理器映射
    std::unordered_map<uint32_t, std::unique_ptr<IMessageHandler>> handlers_;

    /// 控制平面消息处理器
    std::unique_ptr<IMessageHandler> control_plane_handler_;

    /// 下一个 call_id
    std::atomic<uint64_t> next_call_id_{1};

    /// 运行状态
    std::atomic<bool> running_{false};

    /// 退出码
    std::atomic<DasResult> exit_code_{DAS_S_OK};

    /// pending_calls_ 的互斥锁
    mutable std::mutex pending_mutex_;

    /// ConnectionManager for MainProcess mode (nullptr in Host mode)
    std::unique_ptr<ConnectionManager> connection_manager_;

private:
    /// 默认构造函数（禁止直接调用，使用 Create() 代替）
    IpcRunLoop() = default;

    /// 内部初始化（由 Create() 调用）
    DasResult DoInitialize();

    /// 关闭（析构函数调用）
    void Uninitialize();

    /// 标记是否已关闭
    bool is_shutdown_ = false;
};

//=============================================================================
// SendMessageAsync 实现（必须在头文件中，因为返回 auto 类型）
//=============================================================================

inline stdexec::sender auto IpcRunLoop::SendMessageAsync(
    const ValidatedIPCMessageHeader& request_header,
    const uint8_t*                   body,
    size_t                           body_size,
    std::chrono::milliseconds        timeout)
{
    // IpcRunLoop 不再持有内部 transport
    // 调用者必须使用带 transport 参数的版本: SendMessageAsync(transport, ...)
    (void)request_header;
    (void)body;
    (void)body_size;
    (void)timeout;

    // 返回一个立即失败的 sender
    return AwaitResponseSender{
        nullptr,
        static_cast<uint64_t>(DAS_E_IPC_NOT_INITIALIZED),
        std::chrono::milliseconds{0}};
}

inline stdexec::sender auto IpcRunLoop::SendMessageAsync(
    DefaultAsyncIpcTransport*        transport,
    const ValidatedIPCMessageHeader& request_header,
    const uint8_t*                   body,
    size_t                           body_size,
    std::chrono::milliseconds        timeout)
{
    if (!transport)
    {
        return AwaitResponseSender{
            nullptr,
            static_cast<uint64_t>(DAS_E_INVALID_ARGUMENT),
            std::chrono::milliseconds{0}};
    }

    auto [send_result, call_id] = PrepareSendRequestWithTransport(
        transport,
        request_header,
        body,
        body_size);

    if (send_result != DAS_S_OK)
    {
        return AwaitResponseSender{
            nullptr,
            static_cast<uint64_t>(send_result),
            std::chrono::milliseconds{0}};
    }

    return AwaitResponseSender{this, call_id, timeout};
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RUN_LOOP_H
