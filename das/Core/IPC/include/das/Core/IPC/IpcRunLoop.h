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

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/execution.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/IpcResponseSender.h>

DAS_CORE_IPC_NS_BEGIN

class IMessageHandler;

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
    IpcRunLoop();
    ~IpcRunLoop();

    /// 默认初始化（使用内部创建的 transport）
    DasResult Initialize();

    /// 使用指定的队列名称初始化（用于 Host 进程）
    /// @param read_queue_name 读取队列名称
    /// @param write_queue_name 写入队列名称
    /// @param is_server 是否作为服务端（创建队列）
    DasResult Initialize(
        const std::string& read_queue_name,
        const std::string& write_queue_name,
        bool               is_server);

    DasResult Shutdown();

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
     * @param transport 目标传输层
     * @param request_header 请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @param timeout 超时时间（默认30秒）
     * @return stdexec::sender 包含 pair<DasResult, vector<uint8_t>>
     */
    [[nodiscard]]
    stdexec::sender auto SendMessageAsync(
        DefaultAsyncIpcTransport*        transport,
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

    /// 获取 io_context 引用（用于 HostLauncher 等需要共享 io_context 的场景）
    /// 注意：仅在 Initialize() 成功后可用
    boost::asio::io_context& GetIoContext()
    {
        return *io_context_;
    }

    friend class ::Das::Core::IPC::Host::HandshakeHandler;

    // AwaitResponseOperation 需要访问内部方法
    template <class Receiver>
    friend struct AwaitResponseOperation;

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

    //=========================================================================
    // 事件驱动方法（内部使用）
    //=========================================================================

    /**
     * @brief 启动异步接收链
     *
     * 使用 transport 的异步接收能力，注册持续的消息接收回调。
     */
    void StartAsyncReceive();

    /**
     * @brief 调度超时检查定时器
     *
     * 寏隔一段时间检查 pending senders 是否超时。
     */
    void ScheduleTimeoutCheck();

    /**
     * @brief 内部辅助：准备发送请求（分配 call_id、注册 pending
     * 上下文、发送数据）
     *
     * @param request_header 原始请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @return pair<DasResult, uint64_t>: 结果码和分配的 call_id
     */
    std::pair<DasResult, uint64_t> PrepareSendRequest(
        const ValidatedIPCMessageHeader& request_header,
        const uint8_t*                   body,
        size_t                           body_size);

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

    /// 异步传输层（编译期平台选择：Win32AsyncIpcTransport 或
    /// UnixAsyncIpcTransport）
    std::unique_ptr<DefaultAsyncIpcTransport> async_transport_;

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

    /// 下一个 call_id
    std::atomic<uint64_t> next_call_id_{1};

    /// 运行状态
    std::atomic<bool> running_{false};

    /// 退出码
    std::atomic<DasResult> exit_code_{DAS_S_OK};

    /// pending_calls_ 的互斥锁
    mutable std::mutex pending_mutex_;
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
    // 1. 准备发送（分配 call_id、注册 pending、发送数据）
    auto [send_result, call_id] =
        PrepareSendRequest(request_header, body, body_size);

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
