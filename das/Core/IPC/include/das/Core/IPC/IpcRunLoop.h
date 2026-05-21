#ifndef DAS_CORE_IPC_IPC_RUN_LOOP_H
#define DAS_CORE_IPC_IPC_RUN_LOOP_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/Core/Utils/StdExecution.h>
#include <das/IDasAsyncCallback.h>
#include <das/IDasBase.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/DefaultAsyncIpcTransport.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/DasPtr.hpp>

DAS_CORE_IPC_NS_BEGIN

class IMessageHandler;
class IAwaitableMessageHandler;
class ConnectionManager;
class IpcRunLoop; // Forward declaration for templates
class ProxyFactory;
class RemoteObjectRegistry;

namespace Host
{
    class HandshakeHandler;
}

DAS_CORE_IPC_NS_END

DAS_CORE_IPC_NS_BEGIN

/// 异步 pending call 的完成回调类型
/// @param result IPC 结果
/// @param response 响应体
/// @param response_flags 响应头 flags（MessageFlags）
using PendingCallCompletion = std::function<
    void(DasResult, std::vector<uint8_t>, uint16_t response_flags)>;

/// 用于匹配请求和响应的二元组 key
struct CallKey
{
    uint16_t source_session_id; // 消息来源 session
    uint16_t call_id;           // 请求/响应配对 ID

    bool operator==(const CallKey& other) const noexcept
    {
        return source_session_id == other.source_session_id
               && call_id == other.call_id;
    }
};

/// CallKey 哈希函数
struct CallKeyHash
{
    size_t operator()(const CallKey& k) const noexcept
    {
        // 组合成 32-bit 值作为 hash
        return (static_cast<size_t>(k.source_session_id) << 16)
               | static_cast<size_t>(k.call_id);
    }
};

/// 异步 pending call 状态
struct PendingCallState
{
    CallKey                               call_key;
    std::chrono::steady_clock::time_point deadline;
    PendingCallCompletion                 on_complete;
    uint16_t                              response_flags = 0; ///< 响应头 flags
};

//=============================================================================
// AwaitResponseSender — 真正的异步 IPC 响应等待 sender
//=============================================================================

/**
 * @brief AwaitResponseSender 的 OperationState
 *
 * start() 时调用 PostSend（带回调），由事件循环驱动完成（零轮询）。
 * pending_calls_ 的注册在 IO 线程上完成，并且发生在发送之前。
 */
template <class Receiver>
struct AwaitResponseOperation
{
    IpcRunLoop*               loop_;
    ValidatedIPCMessageHeader header_;
    std::vector<uint8_t>      body_;
    CallKey                   call_key_;
    std::chrono::milliseconds timeout_;
    Receiver                  rcvr_;
};

// tag_invoke 实现在 IpcRunLoop 完整定义之后（见文件末尾），避免 clang
// 对不完整类型的检查

/**
 * @brief 异步等待 IPC 响应的 sender
 *
 * 完成时携带 tuple<DasResult, vector<uint8_t>, uint16_t flags>。
 * 不阻塞调用线程，由消息循环事件驱动完成。
 */
struct AwaitResponseSender
{
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(
            std::tuple<DasResult, std::vector<uint8_t>, uint16_t>)>;

    IpcRunLoop*               loop_;
    ValidatedIPCMessageHeader header_;
    std::vector<uint8_t>      body_;
    CallKey                   call_key_;
    std::chrono::milliseconds timeout_;

    template <class Receiver>
    friend auto tag_invoke(
        stdexec::connect_t,
        AwaitResponseSender self,
        Receiver            rcvr) noexcept -> AwaitResponseOperation<Receiver>
    {
        return {
            self.loop_,
            self.header_,
            std::move(self.body_),
            self.call_key_,
            self.timeout_,
            std::move(rcvr)};
    }
};

//=============================================================================
// IpcRunLoop — 异步 IPC 运行时
//=============================================================================

class IpcRunLoop
{
public:
    /**
     * @brief RAII 构造函数
     *
     * 构造即完成初始化：创建 io_context、ConnectionManager，注册所有 IPC stub
     * handlers。
     *
     * @param enable_heartbeat 是否启用心跳线程（调试时可禁用，避免超时杀进程）
     * @param inbound_queue 入站消息队列指针（非持有，由 IpcContext 管理），默认
     * nullptr
     * @param proxy_factory ProxyFactory 引用（由 IpcContext 持有）
     * @param registry RemoteObjectRegistry 引用（由 IpcContext 持有）
     */
    explicit IpcRunLoop(
        bool                             enable_heartbeat,
        IpcMessageQueue<InboundMessage>* inbound_queue,
        ProxyFactory&                    proxy_factory,
        RemoteObjectRegistry&            registry);

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
     * @brief 注册消息处理器（通过 AddRef 管理生命周期）
     *
     * IpcRunLoop 通过 DasPtr 持有 handler 引用（内部调用 AddRef）。
     * 调用方仍持有原始所有权。
     *
     * @param header_flags 消息头标志
     * @param interface_id 接口 ID
     * @param handler 处理器指针（IpcRunLoop 通过 AddRef 延长生命周期）
     */
    void RegisterHandler(
        uint8_t          header_flags,
        uint32_t         interface_id,
        IMessageHandler* handler);

    /**
     * @brief 注册可等待消息处理器（协程版本，控制平面 handler 用）
     *
     * 控制平面 handler（如 HandshakeHandler）使用此接口注册，
     * 支持通过协程异步发送响应。
     * IpcRunLoop 通过 DasPtr 持有 handler 引用（内部调用 AddRef）。
     *
     * @param header_flags 消息头标志
     * @param interface_id 接口 ID
     * @param handler 处理器指针（IpcRunLoop 通过 AddRef 延长生命周期）
     */
    void RegisterHandler(
        uint8_t                   header_flags,
        uint32_t                  interface_id,
        IAwaitableMessageHandler* handler);

    /**
     * @brief 按 header_flags + interface_id 查找处理器
     * @param header_flags 消息头标志
     * @param interface_id 接口 ID
     * @return 处理器指针，未找到返回 nullptr
     */
    [[nodiscard]]
    IMessageHandler* GetHandler(uint8_t header_flags, uint32_t interface_id)
        const;

    /**
     * @brief 无 transport 的 SendMessageAsync 已删除
     *
     * IpcRunLoop 不再持有内部 transport。
     * 调用者必须使用带 transport 参数的版本。
     */
    [[nodiscard]]
    stdexec::sender auto SendMessageAsync(
        const ValidatedIPCMessageHeader& request_header,
        const uint8_t*                   body,
        size_t                           body_size,
        std::chrono::milliseconds timeout = std::chrono::seconds(30)) = delete;

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
        DefaultAsyncIpcTransport* transport DAS_LIFETIMEBOUND,
        const ValidatedIPCMessageHeader&    request_header,
        const uint8_t*                      body,
        size_t                              body_size,
        std::chrono::milliseconds           timeout = std::chrono::seconds(30));

    bool IsRunning() const;

    /**
     * @brief 设置本地 session_id
     *
     * 在 Host 模式下，握手完成后需要更新本地 session_id，
     * 以确保发送请求时使用正确的 source_session_id。
     *
     * @param session_id 新的本地 session_id
     */
    void SetSessionId(uint16_t session_id);

    /// @brief 获取本地 session_id
    /// @return 本地 session_id
    [[nodiscard]]
    uint16_t GetSessionId() const noexcept
    {
        return local_session_id_;
    }

    /// 获取 io_context 引用（用于 HostLauncher 等需要共享 io_context 的场景）
    /// 注意：构造完成后可用
    /// @return io_context 引用，生命周期绑定到 this
    boost::asio::io_context& GetIoContext() DAS_LIFETIMEBOUND
    {
        return *io_context_;
    }

    /**
     * @brief 获取 ConnectionManager（MainProcess 模式）
     *
     * ConnectionManager 用于管理 HostLauncher 实例。
     * 在 Host 模式下 connection_manager_ 为空，解引用将导致 UB。
     *
     * @return ConnectionManager& 引用，生命周期绑定到 this
     */
    ConnectionManager& GetConnectionManager() DAS_LIFETIMEBOUND
    {
        return *connection_manager_;
    }

    /**
     * @brief 投递发送任务到 IO 线程（业务线程调用）
     *
     * 线程安全，任何线程可调用。
     * 将发送任务投递到 IO 线程执行。
     * 如果提供了 on_complete，IO 线程会在发送前注册 pending call，
     * 避免响应先于 pending call completion 挂载而到达。
     *
     * @param header 消息头
     * @param body 消息体（会被 move）
     * @param on_complete 可选的完成回调（非空时注册 pending call）
     * @param deadline 超时截止时间（仅 on_complete 非空时有效）
     * @return DasResult 投递结果
     */
    DasResult PostSend(
        const ValidatedIPCMessageHeader&      header,
        std::vector<uint8_t>&&                body,
        PendingCallCompletion                 on_complete = nullptr,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max());

    /**
     * @brief 投递发送任务到 IO 线程（指定 transport，IO 线程调用）
     *
     * 使用指定的 transport 发送消息，通过发送队列序列化。
     * 所有通过同一 transport 的发送必须经过此方法（或 PostSend），
     * 以防止多个 async_write 并发写入同一管道。
     *
     * @param transport 目标传输层
     * @param header 消息头
     * @param body 消息体（会被 move）
     * @return DasResult 投递结果
     */
    DasResult PostSendWithTransport(
        DefaultAsyncIpcTransport*        transport,
        const ValidatedIPCMessageHeader& header,
        std::vector<uint8_t>&&           body);

    /**
     * @brief Fire-and-forget enqueue to BusinessThread.
     *
     * Wraps callback in an ASYNC_CALLBACK inbound message and pushes it to
     * the BusinessThread queue via the existing inbound_queue_ channel.
     * This reports only immediate enqueue failure; it does not wait for
     * or report callback execution result.
     *
     * @param callback IDasAsyncCallback to execute on BusinessThread
     * @return DasResult DAS_S_OK on success, error on enqueue failure
     *
     * @thread_safety May be called from any thread that owns a proxy
     * reference.
     * @architecture Bridges non-BT proxy teardown to the BusinessThread
     * runtime domain without exposing DOM/Registry to the caller.
     */
    DasResult PostToBusinessThread(IDasAsyncCallback* callback) noexcept;

    /**
     * @brief 注册 HostLauncher 并启动接收循环
     *
     * 在 HostLauncher::Start() 成功后调用。
     * Transport 将在注册后立即开始接收消息。
     *
     * @param launcher HostLauncher 实例（DasPtr 内部用 DasPtr 管理生命周期）
     * @return DasResult DAS_S_OK 成功
     */
    DasResult RegisterHostLauncher(DasPtr<HostLauncher> launcher);

    /**
     * @brief 直接注册 Transport（无 HostLauncher）
     *
     * HTTP/WebSocket 传输模式下，Host 进程独立连接，无需 HostLauncher。
     * 注册到 ConnectionManager 并启动异步接收协程。
     *
     * @param session_id 会话 ID
     * @param transport 要注册的 transport（调用方管理生命周期）
     * @return DasResult DAS_S_OK 成功
     */
    DasResult RegisterDirectTransport(
        uint16_t                 session_id,
        Win32AsyncIpcTransport&& t);
    DasResult RegisterDirectTransport(
        uint16_t                session_id,
        UnixAsyncIpcTransport&& t);
    DasResult RegisterDirectTransport(
        uint16_t           session_id,
        HttpIpcTransport&& t);

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
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        DefaultAsyncIpcTransport&        transport);

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
        uint16_t             session_id,
        DasPtr<HostLauncher> launcher);

    /**
     * @brief 直接传输层的接收协程循环
     *
     * @param session_id 会话 ID
     * @param transport AnyTransport 指针（非拥有）
     */
    boost::asio::awaitable<void> DirectTransportReceiveLoop(
        uint16_t      session_id,
        AnyTransport* transport);

    /**
     * @brief 调度超时检查定时器
     *
     * 寐隔一段时间检查 pending senders 是否超时。
     */
    void ScheduleTimeoutCheck();

    /**
     * @brief 为异步发送准备请求头和 CallKey（不发送、不注册 pending call）
     *
     * 分配 call_id，构建完整的 ValidatedIPCMessageHeader。
     * AwaitResponseSender::start() 随后会通过 PostSend 在 IO 线程上
     * 先注册 completion，再执行发送。
     *
     * @param request_header 原始请求头模板
     * @return pair<ValidatedIPCMessageHeader, CallKey>
     */
    std::pair<ValidatedIPCMessageHeader, CallKey> PrepareAsyncSendHeader(
        const ValidatedIPCMessageHeader& request_header);

    /**
     * @brief 使用指定 transport 准备发送请求
     *
     * @param transport 目标传输层
     * @param request_header 原始请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @return pair<DasResult, CallKey>: 结果码和分配的 CallKey
     */
    std::pair<DasResult, CallKey> PrepareSendRequestWithTransport(
        DefaultAsyncIpcTransport*        transport,
        const ValidatedIPCMessageHeader& request_header,
        const uint8_t*                   body,
        size_t                           body_size);

    /**
     * @brief 完成指定 CallKey 的 pending call
     *
     * 从 pending_calls_ 中取出 on_complete 回调并调用，
     * 在 mutex 外执行回调以避免死锁。
     *
     * @param call_key CallKey (source_session_id, call_id)
     * @param result 结果码
     * @param response 响应体
     */
    void CompletePendingCall(
        CallKey              call_key,
        DasResult            result,
        std::vector<uint8_t> response,
        uint16_t             response_flags);

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

    //=========================================================================
    // 成员变量
    //=========================================================================

    /// pending calls 映射
    std::unordered_map<CallKey, PendingCallState, CallKeyHash> pending_calls_;

    /// 本地 session_id（MainProcess=1, Host 从 Handshake 获取）
    uint16_t local_session_id_ = 0;

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
    std::optional<WorkGuard> work_guard_;

    /// 消息处理器映射（按 header_flags + interface_id 索引）
    std::unordered_map<
        uint8_t,
        std::unordered_map<uint32_t, DasPtr<IMessageHandler>>>
        handlers_by_flags_;

    /// 可等待消息处理器映射（协程版本，控制平面 handler 用）
    /// 使用 DasPtr 管理引用计数，控制平面 handler 使用协程直接发送响应
    std::unordered_map<
        uint8_t,
        std::unordered_map<uint32_t, DasPtr<IAwaitableMessageHandler>>>
        awaitable_handlers_;

    /// 下一个 call_id
    std::atomic<uint16_t> next_call_id_{1};

public:
    /// @brief 分配全局唯一的 call_id
    /// @return 16-bit call_id，0 表示无效（不会返回 0）
    /// @note 线程安全，使用 atomic 递增。溢出后从 1 重新开始。
    [[nodiscard]]
    uint16_t AllocateCallId() noexcept
    {
        uint16_t id = next_call_id_.fetch_add(1);
        // 溢出后从 1 重新开始（0 表示无效）
        if (id == 0)
        {
            id = next_call_id_.fetch_add(1);
            if (id == 0)
            {
                id = 1;
            }
        }
        return id;
    }

    /// 运行状态
    std::atomic<bool> running_{false};

    /// 是否启用心跳线程
    bool enable_heartbeat_ = true;

    /// 退出码
    std::atomic<DasResult> exit_code_{DAS_S_OK};

    /// pending_calls_ 的互斥锁
    mutable std::mutex pending_mutex_;

    /// ConnectionManager for MainProcess mode (nullptr in Host mode)
    std::unique_ptr<ConnectionManager> connection_manager_;

    /// Last time SHM stale block cleanup was triggered
    std::chrono::steady_clock::time_point last_shm_cleanup_time_ =
        std::chrono::steady_clock::now();

    /// 入站消息队列指针（非持有，由 IpcContext 管理）
    /// 通过构造函数注入，不可变
    IpcMessageQueue<InboundMessage>* inbound_queue_ = nullptr;

    /// ProxyFactory 引用（非持有，由 IpcContext 管理）
    /// 用于传递给控制平面 handler 的 StubContext
    ProxyFactory& proxy_factory_;

    /// RemoteObjectRegistry 引用（非持有，由 IpcContext 管理）
    /// 用于传递给控制平面 handler 的 StubContext
    RemoteObjectRegistry& registry_;

private:
    /// @brief 发送失败时构造失败 RESPONSE 并推入 inbound_queue_
    /// @param header 原始请求的 header（用于获取 call_id, session_id）
    /// @param error_code 失败原因的错误码
    void NotifySendFailure(
        const ValidatedIPCMessageHeader& header,
        DasResult                        error_code);

    /// @brief 通过协程发送 IPC 消息（内部共用实现）
    ///
    /// 封装 SendCoroutine 调用、连接检查和错误处理。
    /// launcher 用于延长 transport 生命周期（心跳线程可能并发销毁
    /// HostLauncher）。
    ///
    /// @param transport 目标传输层（fallback，Host 模式下使用）
    /// @param launcher HostLauncher（可能为空，用于保持 transport 存活）
    /// @param header 消息头
    /// @param body 消息体
    boost::asio::awaitable<void> DoSendCoroutine(
        DefaultAsyncIpcTransport* transport,
        DasPtr<HostLauncher>      launcher,
        ValidatedIPCMessageHeader header,
        std::vector<uint8_t>      body);

    /// @brief 按 session_id 查找 transport 并发送（由 PostSend 委托）
    ///
    /// 从 ConnectionManager 查找 launcher/transport，发送前注册
    /// pending call（IO 线程内完成，消除响应早于注册的竞态）。
    ///
    /// @param header 消息头
    /// @param body 消息体
    /// @param on_complete 可选的完成回调（非空时注册 pending call）
    /// @param deadline 超时截止时间
    boost::asio::awaitable<void> SendToSessionCoroutine(
        ValidatedIPCMessageHeader             header,
        std::vector<uint8_t>                  body,
        PendingCallCompletion                 on_complete,
        std::chrono::steady_clock::time_point deadline);
};

//=============================================================================
// AwaitResponseOperation::tag_invoke 实现（需要 IpcRunLoop 完整定义）
//=============================================================================

template <class Receiver>
void tag_invoke(
    stdexec::start_t,
    AwaitResponseOperation<Receiver>& self) noexcept
{
    // 错误路径：loop_ 为 nullptr 表示构造时已检测到错误
    if (!self.loop_)
    {
        auto error_code = static_cast<DasResult>(self.call_key_.call_id);
        stdexec::set_value(
            std::move(self.rcvr_),
            std::make_tuple(error_code, std::vector<uint8_t>{}, uint16_t{0}));
        return;
    }

    auto deadline = std::chrono::steady_clock::now() + self.timeout_;

    // PostSend 带回调：IO 线程内注册 pending_calls_ 后再发送，
    // 消除响应早于 completion 挂载的竞态。
    static_cast<void>(self.loop_->PostSend(
        self.header_,
        std::move(self.body_),
        [rcvr = std::move(self.rcvr_)](
            DasResult            result,
            std::vector<uint8_t> response,
            uint16_t             response_flags) mutable
        {
            stdexec::set_value(
                std::move(rcvr),
                std::make_tuple(result, std::move(response), response_flags));
        },
        deadline));
}

//=============================================================================
// SendMessageAsync 实现（必须在头文件中，因为返回 auto 类型）
//=============================================================================

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
            ValidatedIPCMessageHeader{},
            {},
            CallKey{0, static_cast<uint16_t>(DAS_E_INVALID_ARGUMENT)},
            std::chrono::milliseconds{0}};
    }

    // 分配 call_id 并构建完整的请求头（不发送、不注册 pending call）
    auto [validated_header, call_key] = PrepareAsyncSendHeader(request_header);

    // 构造 body vector，AwaitResponseSender::start() 会通过 PostSend
    // 在 IO 线程上注册 pending call 并发送
    std::vector<uint8_t> body_vec(body, body + body_size);

    return AwaitResponseSender{
        this,
        validated_header,
        std::move(body_vec),
        call_key,
        timeout};
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RUN_LOOP_H
