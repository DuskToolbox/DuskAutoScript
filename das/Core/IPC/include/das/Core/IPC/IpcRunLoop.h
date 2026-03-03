#ifndef DAS_CORE_IPC_IPC_RUN_LOOP_H
#define DAS_CORE_IPC_IPC_RUN_LOOP_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/IDasBase.h>
#include <functional>
#include <optional>
#include <queue>

#include <memory>
#include <mutex>
#include <stdexec/execution.hpp>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <das/Core/IPC/Config.h>

// Forward declarations
DAS_CORE_IPC_NS_BEGIN
// Forward declarations
class IMessageHandler;
class IpcTransport;

namespace Host
{
    class HandshakeHandler;
}
DAS_CORE_IPC_NS_END

DAS_CORE_IPC_NS_BEGIN

/// 异步 pending call 的完成回调类型
using PendingCallCompletion =
    std::function<void(DasResult, std::vector<uint8_t>)>;

/**
 * @brief 异步 pending call 状态（IOCP 风格，替代原 NestedCallContext）
 *
 * 每个 pending call 记录：
 * - call_id: 调用 ID
 * - response_buffer: 响应体（由 ProcessMessage 填入）
 * - deadline: 超时截止时间
 * - on_complete: 完成/超时时的回调
 */
struct PendingCallState
{
    uint64_t                              call_id;
    std::vector<uint8_t>                  response_buffer;
    std::chrono::steady_clock::time_point deadline;
    PendingCallCompletion                 on_complete;
};

/// 投递的回调类型
using PostedCallback = std::function<void()>;

//=============================================================================
// IpcScheduler — stdexec scheduler，基于 PostMessage 实现
//=============================================================================

class IpcRunLoop; // forward

/**
 * @brief ScheduleSender 的 OperationState
 *
 * start() 时通过 PostMessage 将 set_value 投递到 RunLoop 线程。
 */
template <class Receiver>
struct IpcScheduleOperation
{
    IpcRunLoop* loop_;
    Receiver    rcvr_;

    friend void tag_invoke(
        stdexec::start_t,
        IpcScheduleOperation& self) noexcept
    {
        self.loop_->PostMessage([rcvr = std::move(self.rcvr_)]() mutable
                                { stdexec::set_value(std::move(rcvr)); });
    }
};

/**
 * @brief IpcScheduler::schedule() 返回的 sender
 *
 * connect 后产生 IpcScheduleOperation，start 时投递到 RunLoop 线程。
 */
struct IpcScheduleSender
{
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t()>;

    IpcRunLoop* loop_;

    template <class Receiver>
    friend auto tag_invoke(
        stdexec::connect_t,
        IpcScheduleSender self,
        Receiver          rcvr) noexcept -> IpcScheduleOperation<Receiver>
    {
        return {self.loop_, std::move(rcvr)};
    }
};

/**
 * @brief stdexec scheduler，将工作投递到 IpcRunLoop 线程
 *
 * 用法：
 *   auto sched = run_loop.GetScheduler();
 *   auto work = stdexec::schedule(sched)
 *             | stdexec::then([] {  在 RunLoop 线程上执行 });
 */
struct IpcScheduler
{
    using scheduler_concept = stdexec::scheduler_t;

    IpcRunLoop* loop_;

    friend IpcScheduleSender tag_invoke(
        stdexec::schedule_t,
        IpcScheduler self) noexcept
    {
        return IpcScheduleSender{self.loop_};
    }

    friend bool operator==(IpcScheduler a, IpcScheduler b) noexcept
    {
        return a.loop_ == b.loop_;
    }
    friend bool operator!=(IpcScheduler a, IpcScheduler b) noexcept
    {
        return a.loop_ != b.loop_;
    }
};

//=============================================================================
// AwaitResponseSender — 真正的异步 IPC 响应等待 sender
//=============================================================================

/**
 * @brief AwaitResponseSender 的 OperationState
 *
 * start() 时注册 on_complete 到 PendingCallState，
 * 由 RunInternal 的消息循环驱动完成（零轮询）。
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
        // 错误路径：loop_ 为 nullptr 表示 PrepareSendRequest 已失败，
        // call_id_ 实际上存储的是错误码（通过 reinterpret）
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
        // 回调会在 RunLoop 线程上被调用
        // （由 ProcessMessage 或 TickPendingSenders 触发）
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

// 内部类型，不对外导出
class IpcRunLoop
{
public:
    IpcRunLoop();
    ~IpcRunLoop();

    DasResult Initialize();
    DasResult Shutdown();
    // 阻塞式消息循环
    DasResult Run();

    DasResult Stop();

    /**
     * @brief 仅设置 running_ 标志为 false，不 join 线程
     *
     * 用于在 io_thread_ 内部（如 GOODBYE 回调中）安全请求退出，
     * 避免在 io_thread_ 上调用 join 导致死锁。
     * 线程的 join 由 Run() 或 Stop() 完成。
     */
    void RequestStop();

    void SetTransport(std::unique_ptr<IpcTransport> transport);

    IpcTransport* GetTransport() const;

    // 等待消息循环结束
    DasResult WaitForShutdown();

    /**
     * @brief 注册消息处理器
     * @param handler 处理器实例（所有权转移）
     */
    void RegisterHandler(std::unique_ptr<IMessageHandler> handler);

    /**
     * @brief 按接口 ID 查找处理器
     * @param interface_id 接口 ID
     * @return 处理器指针，未找到返回 nullptr
     */
    [[nodiscard]]
    IMessageHandler* GetHandler(uint32_t interface_id) const;

    /**
     * @brief 同步阻塞 IPC 调用（使用内部 transport）
     *
     * 发送请求后进入消息循环等待，支持可重入调用。
     * 内部使用 ReceiveAndDispatch() 处理消息。
     *
     * @param request_header 请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @param response_body [out] 响应体
     * @param timeout 超时时间（默认30秒）
     * @return 调用结果
     */
    DasResult SendRequest(
        const ValidatedIPCMessageHeader& request_header,
        const uint8_t*                   body,
        size_t                           body_size,
        std::vector<uint8_t>&            response_body,
        std::chrono::milliseconds        timeout = std::chrono::seconds(30));

    /**
     * @brief 同步阻塞 IPC 调用（指定 transport）
     *
     * 用于主进程转发场景：可以在处理 A 的消息时，
     * 使用 Transport_B 发送请求到 Host B。
     *
     * @param transport 指定使用的传输层
     * @param request_header 请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @param response_body [out] 响应体
     * @param timeout 超时时间（默认30秒）
     * @return 调用结果
     */
    DasResult SendRequest(
        IpcTransport*                    transport,
        const ValidatedIPCMessageHeader& request_header,
        const uint8_t*                   body,
        size_t                           body_size,
        std::vector<uint8_t>&            response_body,
        std::chrono::milliseconds        timeout = std::chrono::seconds(30));

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
     * @brief 异步 IPC 调用（指定 transport，返回 sender）
     */
    [[nodiscard]]
    stdexec::sender auto SendMessageAsync(
        IpcTransport*                    transport,
        const ValidatedIPCMessageHeader& request_header,
        const uint8_t*                   body,
        size_t                           body_size,
        std::chrono::milliseconds        timeout = std::chrono::seconds(30));

    DasResult SendResponse(
        const ValidatedIPCMessageHeader& response_header,
        const uint8_t*                   body,
        size_t                           body_size);

    DasResult SendEvent(
        const ValidatedIPCMessageHeader& event_header,
        const uint8_t*                   body,
        size_t                           body_size);

    bool IsRunning() const;

    //=========================================================================
    // Scheduler API
    //=========================================================================

    /**
     * @brief 获取 stdexec scheduler
     *
     * 返回的 scheduler 可用于 stdexec::schedule()、stdexec::on() 等，
     * 将工作投递到 IpcRunLoop 线程执行。
     */
    IpcScheduler GetScheduler() noexcept { return IpcScheduler{this}; }

    //=========================================================================
    // PostMessage API
    //=========================================================================

    /// 投递一个回调到 RunLoop 线程执行
    /// 线程安全，可从任意线程调用
    /// @param callback 要执行的回调
    void PostMessage(PostedCallback callback);

    /// 投递"启动 Host"任务
    /// RunLoop 会管理 HostLauncher 的生命周期
    /// @param plugin_path 插件 manifest 路径
    /// @param on_complete 完成回调（可选）
    void PostStartHost(
        const std::string&                                         plugin_path,
        std::function<void(DasResult result, uint16_t session_id)> on_complete =
            nullptr);

    /// 投递"停止 Host"任务
    /// @param session_id 要停止的 Host 的 session_id
    void PostStopHost(uint16_t session_id);
    friend class ::Das::Core::IPC::Host::HandshakeHandler;

    // AwaitResponseOperation 需要访问内部方法
    template <class Receiver>
    friend struct AwaitResponseOperation;

    // IpcScheduleOperation 需要访问 PostMessage
    template <class Receiver>
    friend struct IpcScheduleOperation;

    //=========================================================================
    // PostMessage 内部实现
    //=========================================================================

    /// 发送唤醒消息（内部使用）
    void SendWakeupMessage();

    /// 处理投递的回调（由 HandshakeHandler 调用）
    void ProcessPostedCallbacks();

    DasResult ProcessMessage(
        const IPCMessageHeader& header,
        const uint8_t*          body,
        size_t                  body_size);

    /**
     * @brief 分发消息到注册的处理器
     */
    DasResult DispatchToHandler(
        const IPCMessageHeader&     header,
        const std::vector<uint8_t>& body);

    /**
     * @brief 内部 receive 方法 - 核心可重入逻辑
     */
    bool ReceiveAndDispatch(std::chrono::milliseconds timeout);

    /**
     * @brief 使用指定 transport 的 receive 方法 - 用于转发场景
     *
     * 在主进程转发场景中，需要在处理 Transport_A 的消息时，
     * 使用 Transport_B 发送请求。此方法允许指定 transport 接收消息。
     *
     * @param transport 指定的传输层
     * @param timeout 超时时间
     * @return 是否收到并处理了消息
     */
    bool ReceiveAndDispatchFromTransport(
        IpcTransport*             transport,
        std::chrono::milliseconds timeout);

    void RunInternal();

    /**
     * @brief 内部辅助：准备发送请求（分配 call_id、注册 pending
     * 上下文、发送数据）
     *
     * @param transport 传输层
     * @param request_header 原始请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @return pair<DasResult, uint64_t>: 结果码和分配的 call_id
     */
    std::pair<DasResult, uint64_t> PrepareSendRequest(
        IpcTransport*                    transport,
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
     * 由 RunInternal 在每轮消息循环中调用。
     * 检查所有 pending call 的 deadline，超时的调用
     * on_complete(DAS_E_IPC_TIMEOUT, {})。
     */
    void TickPendingSenders();

    /**
     * @brief 获取最近的 pending call 超时时间（毫秒）
     *
     * 用于 RunInternal 计算 timed_receive 的超时参数。
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

    // 直接成员（移除 pimpl）
    std::unordered_map<uint64_t, PendingCallState> pending_calls_;
    std::unique_ptr<IpcTransport>                  transport_;

    std::unordered_map<uint32_t, std::unique_ptr<IMessageHandler>> handlers_;
    std::atomic<uint64_t>  next_call_id_{1};
    std::atomic<bool>      running_{false};
    std::atomic<DasResult> exit_code_{DAS_S_OK};
    std::thread            io_thread_;
    mutable std::mutex     pending_mutex_;

    //=========================================================================
    // PostMessage 成员
    //=========================================================================

    /// 投递回调队列
    std::mutex                 post_queue_mutex_;
    std::queue<PostedCallback> post_queue_;

    /// 管理的 Host 进程（预留，HostLauncher 在 Wave 5 实现）
    std::mutex hosts_mutex_;
    std::unordered_map<uint16_t, void*>
        hosts_; // 先用 void*，后续替换为 HostLauncher*
};

//=============================================================================
// SendMessageAsync 实现（必须在头文件中，因为返回 auto 类型）
//=============================================================================

// 注意：带 transport 参数的实现版本必须在转发版本之前定义，
//       否则 MSVC 报 C3779（auto 返回函数需先定义再调用）。
inline stdexec::sender auto IpcRunLoop::SendMessageAsync(
    IpcTransport*                    transport,
    const ValidatedIPCMessageHeader& request_header,
    const uint8_t*                   body,
    size_t                           body_size,
    std::chrono::milliseconds        timeout)
{
    // 1. 准备发送（分配 call_id、注册 pending、发送数据）
    auto [send_result, call_id] =
        PrepareSendRequest(transport, request_header, body, body_size);

    // 2. 返回 AwaitResponseSender
    //    - 成功：loop_=this, call_id=实际ID, timeout=超时
    //    - 失败：loop_=nullptr, call_id=错误码(复用), timeout=0
    //    AwaitResponseOperation::start() 中根据 loop_ 是否为 nullptr 区分
    if (send_result != DAS_S_OK)
    {
        return AwaitResponseSender{
            nullptr,
            static_cast<uint64_t>(send_result),
            std::chrono::milliseconds{0}};
    }

    return AwaitResponseSender{this, call_id, timeout};
}

inline stdexec::sender auto IpcRunLoop::SendMessageAsync(
    const ValidatedIPCMessageHeader& request_header,
    const uint8_t*                   body,
    size_t                           body_size,
    std::chrono::milliseconds        timeout)
{
    return SendMessageAsync(
        transport_.get(),
        request_header,
        body,
        body_size,
        timeout);
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RUN_LOOP_H
