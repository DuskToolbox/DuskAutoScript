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

// HeaderFlags 已移至 IpcMessageHeader.h

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

/// V3: 用于匹配请求和响应的二元组 key
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

/// 异步 pending call 状态（IOCP 风格）
struct PendingCallState
{
    CallKey                               call_key;
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
    CallKey                   call_key_;
    std::chrono::milliseconds timeout_;
    Receiver                  rcvr_;

    friend void tag_invoke(
        stdexec::start_t,
        AwaitResponseOperation& self) noexcept
    {
        // 错误路径：loop_ 为 nullptr 表示 PrepareSendRequest 已失败
        if (!self.loop_)
        {
            auto error_code = static_cast<DasResult>(self.call_key_.call_id);
            stdexec::set_value(
                std::move(self.rcvr_),
                std::make_pair(error_code, std::vector<uint8_t>{}));
            return;
        }

        auto deadline = std::chrono::steady_clock::now() + self.timeout_;

        // 注册完成回调到 PendingCallState
        self.loop_->RegisterPendingCompletion(
            self.call_key_,
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
    CallKey                   call_key_;
    std::chrono::milliseconds timeout_;

    template <class Receiver>
    friend auto tag_invoke(
        stdexec::connect_t,
        AwaitResponseSender self,
        Receiver            rcvr) noexcept -> AwaitResponseOperation<Receiver>
    {
        return {self.loop_, self.call_key_, self.timeout_, std::move(rcvr)};
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
     * @param header_flags 消息头标志（HeaderFlags::NONE 或
     * HeaderFlags::CONTROL_PLANE）
     * @param interface_id 接口 ID
     * @param handler 处理器实例（所有权转移）
     */
    void RegisterHandler(
        uint8_t                          header_flags,
        uint32_t                         interface_id,
        std::unique_ptr<IMessageHandler> handler);

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
     * @brief 按接口 ID 查找处理器（兼容接口，查找 NONE 标志的处理器）
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

    /**
     * @brief 设置本地 session_id
     *
     * 在 Host 模式下，握手完成后需要更新本地 session_id，
     * 以确保发送请求时使用正确的 source_session_id。
     *
     * @param session_id 新的本地 session_id
     */
    void SetSessionId(uint16_t session_id);

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
        const ValidatedIPCMessageHeader& header,
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
     * @return pair<DasResult, CallKey>: 结果码和分配的 CallKey
     */
    std::pair<DasResult, CallKey> PrepareSendRequestWithTransport(
        DefaultAsyncIpcTransport*        transport,
        const ValidatedIPCMessageHeader& request_header,
        const uint8_t*                   body,
        size_t                           body_size);

    /**
     * @brief 完成指定 CallKey 的 pending call（IOCP 风格）
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
     * @param call_key CallKey (source_session_id, call_id)
     * @param deadline 超时截止时间
     * @param on_complete 完成回调
     */
    void RegisterPendingCompletion(
        CallKey                               call_key,
        std::chrono::steady_clock::time_point deadline,
        PendingCallCompletion                 on_complete);

    //=========================================================================
    // 成员变量
    //=========================================================================

    /// pending calls 映射 (V3: 使用 CallKey 作为 key)
    std::unordered_map<CallKey, PendingCallState, CallKeyHash> pending_calls_;

    /// 本地 session_id (V3: MainProcess=1, Host 从 Handshake 获取)
    uint16_t local_session_id_ = 1;

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

    /// 消息处理器映射（按 header_flags + interface_id 索引）
    std::unordered_map<
        uint8_t,
        std::unordered_map<uint32_t, DasPtr<IMessageHandler>>>
        handlers_by_flags_;

    /// 下一个 call_id (V3: uint16_t)
    std::atomic<uint16_t> next_call_id_{1};

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
        CallKey{0, static_cast<uint16_t>(DAS_E_IPC_NOT_INITIALIZED)},
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
            CallKey{0, static_cast<uint16_t>(DAS_E_INVALID_ARGUMENT)},
            std::chrono::milliseconds{0}};
    }

    auto [send_result, call_key] = PrepareSendRequestWithTransport(
        transport,
        request_header,
        body,
        body_size);

    if (send_result != DAS_S_OK)
    {
        return AwaitResponseSender{
            nullptr,
            CallKey{0, static_cast<uint16_t>(send_result)},
            std::chrono::milliseconds{0}};
    }

    return AwaitResponseSender{this, call_key, timeout};
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RUN_LOOP_H
