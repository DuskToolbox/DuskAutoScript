#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/IpcTransport.h>
#include <mutex>
#include <stdexec/execution.hpp>
#include <thread>
#include <unordered_map>
#include <utility>

#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>
DAS_CORE_IPC_NS_BEGIN

// 握手消息 interface_id 范围常量
// HandshakeInterfaceId 枚举范围: 1=HELLO, 2=WELCOME, 3=READY, 4=READY_ACK,
// 5=WAKEUP, 6=HEARTBEAT, 7=GOODBYE
static constexpr uint32_t kHandshakeInterfaceIdStart = 1;
static constexpr uint32_t kHandshakeInterfaceIdEnd =
    static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_GOODBYE);

//=========================================================================
// 内部同步等待 receiver（用于 SendRequestAndWait）
//=========================================================================
DAS_NS_ANONYMOUS_DETAILS_BEGIN
struct SyncWaitReceiver
{
    std::optional<std::pair<DasResult, std::vector<uint8_t>>>* result;
    bool* done;

    using receiver_concept = stdexec::receiver_t;

    friend void tag_invoke(
        stdexec::set_value_t,
        SyncWaitReceiver&& self,
        DasResult r,
        std::vector<uint8_t> response)
    {
        self.result->emplace(r, std::move(response));
        *self.done = true;
    }

    friend stdexec::env<> tag_invoke(
        stdexec::get_env_t,
        const SyncWaitReceiver&) noexcept
    {
        return {};
    }
};
DAS_NS_ANONYMOUS_DETAILS_END

IpcRunLoop::IpcRunLoop() = default;

IpcRunLoop::~IpcRunLoop() { Stop(); }

DasResult IpcRunLoop::Initialize()
{
    transport_ = std::make_unique<IpcTransport>();
    return DAS_S_OK;
}

DasResult IpcRunLoop::Shutdown()
{
    Stop();
    transport_.reset();
    return DAS_S_OK;
}

DasResult IpcRunLoop::Stop()
{
    if (!running_.load())
    {
        return DAS_S_OK;
    }

    running_.store(false);

    // 通知所有 pending calls 超时完成
    {
        std::vector<PendingCallCompletion> completions;
        {
            std::unique_lock<std::mutex> lock(pending_mutex_);
            for (auto& [call_id, state] : pending_calls_)
            {
                if (state.on_complete)
                {
                    completions.push_back(std::move(state.on_complete));
                }
            }
            pending_calls_.clear();
        }
        // 在 mutex 外执行回调，避免死锁
        for (auto& cb : completions)
        {
            cb(DAS_E_IPC_TIMEOUT, {});
        }
    }

    // 发送唤醒消息让 Receive 返回，使 RunInternal 退出循环
    SendWakeupMessage();

    if (io_thread_.joinable())
    {
        io_thread_.join();
    }

    return DAS_S_OK;
}

void IpcRunLoop::RequestStop()
{
    running_.store(false);

    // 通知所有 pending calls 完成，避免阻塞
    {
        std::vector<PendingCallCompletion> completions;
        {
            std::unique_lock<std::mutex> lock(pending_mutex_);
            for (auto& [call_id, state] : pending_calls_)
            {
                if (state.on_complete)
                {
                    completions.push_back(std::move(state.on_complete));
                }
            }
            pending_calls_.clear();
        }
        // 在 mutex 外执行回调
        for (auto& cb : completions)
        {
            cb(DAS_E_IPC_TIMEOUT, {});
        }
    }
}

void IpcRunLoop::RegisterHandler(std::unique_ptr<IMessageHandler> handler)
{
    if (handler)
    {
        uint32_t id = handler->GetInterfaceId();
        handlers_[id] = std::move(handler);
    }
}

IMessageHandler* IpcRunLoop::GetHandler(uint32_t interface_id) const
{
    auto it = handlers_.find(interface_id);
    return (it != handlers_.end()) ? it->second.get() : nullptr;
}

DasResult IpcRunLoop::DispatchToHandler(
    const IPCMessageHeader&     header,
    const std::vector<uint8_t>& body)
{
    // 握手消息范围检查（控制平面消息，包括
    // HELLO/WELCOME/READY/READY_ACK/WAKEUP/HEARTBEAT/GOODBYE） 所有这些消息都由
    // HandshakeHandler 处理
    if (header.interface_id >= kHandshakeInterfaceIdStart
        && header.interface_id <= kHandshakeInterfaceIdEnd)
    {
        auto* handshake_handler = GetHandler(
            kHandshakeInterfaceIdStart); // HandshakeHandler::INTERFACE_ID = 1
        if (handshake_handler)
        {
            IpcResponseSender sender(*this);
            return handshake_handler->HandleMessage(header, body, sender);
        }
    }

    // 控制平面消息：method_id = 0，由 IpcCommandHandler 处理
    // 根据 IpcMessageHeader 注释："method_id: 业务方法ID（控制平面固定 0）"
    constexpr uint16_t kControlPlaneMethodId = 0;
    if (header.method_id == kControlPlaneMethodId)
    {
        // IpcCommandHandler 注册在 interface_id = 0
        auto* command_handler = GetHandler(0);
        if (command_handler)
        {
            IpcResponseSender sender(*this);
            return command_handler->HandleMessage(header, body, sender);
        }
    }

    // 业务平面消息：精确匹配 interface_id
    auto* handler = GetHandler(header.interface_id);
    if (handler)
    {
        IpcResponseSender sender(*this);
        return handler->HandleMessage(header, body, sender);
    }

    // No handler found
    DAS_CORE_LOG_WARN(
        "No handler found for interface_id = {}",
        header.interface_id);
    return DAS_E_NOT_FOUND;
}

bool IpcRunLoop::ReceiveAndDispatch(std::chrono::milliseconds timeout)
{
    IPCMessageHeader     header;
    std::vector<uint8_t> body;

    auto result =
        transport_->Receive(header, body, static_cast<int>(timeout.count()));

    if (result == DAS_S_OK)
    {
        // 1. 优先检查是否是响应消息 → 直接完成 pending call
        if (header.message_type == static_cast<uint8_t>(MessageType::RESPONSE))
        {
            CompletePendingCall(header.call_id, DAS_S_OK, std::move(body));
            return true;
        }

        // 2. 分发到注册的处理器
        DispatchToHandler(header, body);
        return true;
    }
    return false;
}

bool IpcRunLoop::ReceiveAndDispatchFromTransport(
    IpcTransport*             transport,
    std::chrono::milliseconds timeout)
{
    if (!transport)
    {
        DAS_CORE_LOG_ERROR("Transport is null");
        return false;
    }

    IPCMessageHeader     header;
    std::vector<uint8_t> body;

    auto result =
        transport->Receive(header, body, static_cast<int>(timeout.count()));

    if (result == DAS_S_OK)
    {
        // 1. 优先检查是否是响应消息 → 直接完成 pending call
        if (header.message_type == static_cast<uint8_t>(MessageType::RESPONSE))
        {
            CompletePendingCall(header.call_id, DAS_S_OK, std::move(body));
            return true;
        }

        // 2. 分发到注册的处理器
        DispatchToHandler(header, body);
        return true;
    }
    return false;
}

std::pair<DasResult, uint64_t> IpcRunLoop::PrepareSendRequest(
    IpcTransport*                    transport,
    const ValidatedIPCMessageHeader& request_header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    if (!transport)
    {
        DAS_CORE_LOG_ERROR("Transport is null");
        return {DAS_E_IPC_INVALID_ARGUMENT, 0};
    }

    uint64_t call_id = next_call_id_.fetch_add(1);

    // 使用 Builder 创建新的 header，设置 call_id
    const IPCMessageHeader& raw = request_header.Raw();
    auto                    validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::REQUEST)
            .SetBusinessInterface(raw.interface_id, raw.method_id)
            .SetBodySize(raw.body_size)
            .SetCallId(call_id)
            .SetObject(raw.session_id, raw.generation, raw.local_id)
            .Build();

    // 创建等待上下文（on_complete 和 deadline 稍后由
    // AwaitResponseOperation::start() 通过 RegisterPendingCompletion 设置）
    {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_[call_id] = PendingCallState{
            call_id,
            {},
            std::chrono::steady_clock::time_point::max(),
            nullptr};
    }

    // 发送请求
    auto send_result = transport->Send(validated_header, body, body_size);
    if (send_result != DAS_S_OK)
    {
        DAS_CORE_LOG_ERROR(
            "Failed to send request (error = 0x{:08X})",
            send_result);
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_.erase(call_id);
        return {send_result, 0};
    }

    return {DAS_S_OK, call_id};
}

//=========================================================================
// IOCP 风格异步 pending call 管理
//=========================================================================

void IpcRunLoop::CompletePendingCall(
    uint64_t             call_id,
    DasResult            result,
    std::vector<uint8_t> response)
{
    PendingCallCompletion on_complete;

    {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        auto                         it = pending_calls_.find(call_id);
        if (it == pending_calls_.end())
        {
            return; // 已被清理（超时或 Stop）
        }

        // 取出回调并移除 pending call
        on_complete = std::move(it->second.on_complete);
        pending_calls_.erase(it);
    }

    // 在 mutex 外执行回调，避免死锁
    if (on_complete)
    {
        on_complete(result, std::move(response));
    }
}

void IpcRunLoop::TickPendingSenders()
{
    auto now = std::chrono::steady_clock::now();

    // 收集所有超时的 pending call
    std::vector<std::pair<uint64_t, PendingCallCompletion>> expired;

    {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        for (auto it = pending_calls_.begin(); it != pending_calls_.end();)
        {
            if (it->second.on_complete && now >= it->second.deadline)
            {
                expired.emplace_back(
                    it->first,
                    std::move(it->second.on_complete));
                it = pending_calls_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // 在 mutex 外执行超时回调
    for (auto& [call_id, cb] : expired)
    {
        cb(DAS_E_IPC_TIMEOUT, {});
    }
}

uint32_t IpcRunLoop::GetNearestDeadlineMs() const
{
    std::unique_lock<std::mutex> lock(pending_mutex_);

    if (pending_calls_.empty())
    {
        return 0; // 无 pending call，使用无限等待
    }

    auto now = std::chrono::steady_clock::now();
    auto nearest = std::chrono::steady_clock::time_point::max();

    for (const auto& [call_id, state] : pending_calls_)
    {
        if (state.on_complete && state.deadline < nearest)
        {
            nearest = state.deadline;
        }
    }

    if (nearest == std::chrono::steady_clock::time_point::max())
    {
        return 0; // 没有设置 deadline 的 pending call
    }

    if (nearest <= now)
    {
        return 1; // 已超时，立即返回（最小 1ms，避免 0 表示无限等待）
    }

    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(nearest - now);
    return std::max(
        static_cast<uint32_t>(1),
        static_cast<uint32_t>(ms.count()));
}

void IpcRunLoop::RegisterPendingCompletion(
    uint64_t                              call_id,
    std::chrono::steady_clock::time_point deadline,
    PendingCallCompletion                 on_complete)
{
    {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        auto                         it = pending_calls_.find(call_id);
        if (it != pending_calls_.end())
        {
            // 设置回调和超时
            it->second.deadline = deadline;
            it->second.on_complete = std::move(on_complete);
            return;
        }
    }

    // call_id 已被清理（Stop/RequestStop 清除），回调超时
    if (on_complete)
    {
        on_complete(DAS_E_IPC_TIMEOUT, {});
    }
}

DasResult IpcRunLoop::SendRequest(
    const ValidatedIPCMessageHeader& request_header,
    const uint8_t*                   body,
    size_t                           body_size,
    std::vector<uint8_t>&            response_body,
    std::chrono::milliseconds        timeout)
{
    if (!transport_)
    {
        DAS_CORE_LOG_ERROR("Transport not initialized");
        return DAS_E_IPC_INVALID_ARGUMENT;
    }
    return SendRequest(
        transport_.get(),
        request_header,
        body,
        body_size,
        response_body,
        timeout);
}

DasResult IpcRunLoop::SendRequest(
    IpcTransport*                    transport,
    const ValidatedIPCMessageHeader& request_header,
    const uint8_t*                   body,
    size_t                           body_size,
    std::vector<uint8_t>&            response_body,
    std::chrono::milliseconds        timeout)
{
    // 构建 stdexec 异步管道并同步等待结果
    auto sender =
        SendMessageAsync(transport, request_header, body, body_size, timeout);

    auto result_opt = stdexec::sync_wait(std::move(sender));
    if (!result_opt.has_value())
    {
        DAS_CORE_LOG_WARN("Async operation timed out or cancelled");
        return DAS_E_IPC_TIMEOUT;
    }

    auto& [result, response] = std::get<0>(result_opt.value());
    response_body = std::move(response);
    return result;
}

DasResult IpcRunLoop::SendResponse(
    const ValidatedIPCMessageHeader& response_header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    return transport_->Send(response_header, body, body_size);
}

DasResult IpcRunLoop::SendEvent(
    const ValidatedIPCMessageHeader& event_header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    // 使用 Builder 确保设置 EVENT 类型
    const IPCMessageHeader& raw = event_header.Raw();
    auto                    validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::EVENT)
            .SetBusinessInterface(raw.interface_id, raw.method_id)
            .SetBodySize(raw.body_size)
            .SetCallId(raw.call_id)
            .SetObject(raw.session_id, raw.generation, raw.local_id)
            .Build();

    return transport_->Send(validated_header, body, body_size);
}

bool IpcRunLoop::IsRunning() const { return running_.load(); }

void IpcRunLoop::SetTransport(std::unique_ptr<IpcTransport> transport)
{
    transport_ = std::move(transport);
}

IpcTransport* IpcRunLoop::GetTransport() const
{
    // 优先返回非拥有指针（测试场景），否则返回拥有指针
    return transport_ptr_ ? transport_ptr_ : transport_.get();
}

void IpcRunLoop::SetTransportPtr(IpcTransport* transport)
{
    transport_ptr_ = transport;
}

void IpcRunLoop::RunInternal()
{
    DasResult exit_code = DAS_S_OK;

    while (running_.load())
    {
        // 1. 计算 timed_receive 的超时：
        //    取最近的 pending call deadline，兼做定时器
        //    返回 0 表示没有 pending call → 无限阻塞等待
        uint32_t timeout_ms = GetNearestDeadlineMs();

        IPCMessageHeader     header;
        std::vector<uint8_t> body;

        auto result = transport_->Receive(header, body, timeout_ms);

        if (result == DAS_E_IPC_TIMEOUT)
        {
            // timed_receive 超时：扫描并完成超时的 pending call
            TickPendingSenders();
            continue;
        }

        if (result != DAS_S_OK)
        {
            exit_code = result;
            break;
        }

        // 2. 处理消息（RESPONSE 会直接触发 on_complete 回调）
        ProcessMessage(header, body.data(), body.size());

        // 3. 顺带检查超时（消息处理可能耗时）
        TickPendingSenders();
    }

    exit_code_.store(exit_code);
}

DasResult IpcRunLoop::ProcessMessage(
    const IPCMessageHeader& header,
    const uint8_t*          body,
    size_t                  body_size)
{
    if (header.message_type == static_cast<uint8_t>(MessageType::RESPONSE))
    {
        // IOCP 风格：直接完成 pending call，触发 on_complete 回调
        std::vector<uint8_t> response_body(body, body + body_size);
        CompletePendingCall(header.call_id, DAS_S_OK, std::move(response_body));
        return DAS_S_OK;
    }

    if (header.message_type == static_cast<uint8_t>(MessageType::REQUEST))
    {
        // 优先使用新处理器系统
        std::vector<uint8_t> body_vec(body, body + body_size);
        auto dispatch_result = DispatchToHandler(header, body_vec);

        // 如果找到处理器，直接返回结果
        if (dispatch_result != DAS_E_NOT_FOUND)
        {
            return dispatch_result;
        }

        // 没有处理器，发送错误响应
        auto validated_response =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::RESPONSE)
                .SetBusinessInterface(header.interface_id, header.method_id)
                .SetBodySize(0)
                .SetCallId(header.call_id)
                .SetObject(
                    header.session_id,
                    header.generation,
                    header.local_id)
                .SetErrorCode(DAS_E_IPC_INVALID_INTERFACE_ID)
                .Build();

        transport_->Send(validated_response, nullptr, 0);
        return DAS_S_OK;
    }

    if (header.message_type == static_cast<uint8_t>(MessageType::EVENT))
    {
        return DAS_S_OK;
    }

    if (header.message_type == static_cast<uint8_t>(MessageType::HEARTBEAT))
    {
        return DAS_S_OK;
    }

    return DAS_E_IPC_INVALID_MESSAGE_TYPE;
}

DasResult IpcRunLoop::Run()
{
    if (running_.load())
    {
        DAS_CORE_LOG_ERROR("Deadlock detected - already running");
        return DAS_E_IPC_DEADLOCK_DETECTED;
    }

    running_.store(true);
    exit_code_.store(DAS_S_OK);
    io_thread_ = std::thread([this]() { this->RunInternal(); });

    // 等待线程完成（阻塞）
    if (io_thread_.joinable())
    {
        io_thread_.join();
    }
    return exit_code_.load();
}

DasResult IpcRunLoop::WaitForShutdown()
{
    if (io_thread_.joinable())
    {
        io_thread_.join();
    }
    return exit_code_.load();
}

//=========================================================================
// PostRequest 实现
//=========================================================================

void IpcRunLoop::PostRequest(PostedCallback callback)
{
    {
        std::lock_guard<std::mutex> lock(post_queue_mutex_);
        post_queue_.push(std::move(callback));
    }

    // 发送唤醒消息，让 Receive() 返回
    SendWakeupMessage();
}

void IpcRunLoop::SendWakeupMessage()
{
    if (!transport_)
    {
        return;
    }

    // 构造唤醒消息（使用 Builder）
    auto validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::REQUEST)
            .SetControlPlaneCommand(
                HandshakeInterfaceId::HANDSHAKE_IFACE_WAKEUP)
            .SetBodySize(0)
            .Build();
    transport_->Send(validated_header, nullptr, 0);
}

void IpcRunLoop::ProcessPostedCallbacks()
{
    std::queue<PostedCallback> local_queue;

    {
        std::lock_guard<std::mutex> lock(post_queue_mutex_);
        local_queue = std::move(post_queue_);
        post_queue_ = std::queue<PostedCallback>();
    }

    while (!local_queue.empty())
    {
        auto callback = std::move(local_queue.front());
        local_queue.pop();
        callback(); // 执行
    }
}

void IpcRunLoop::PostStartHost(
    const std::string&                       plugin_path,
    std::function<void(DasResult, uint16_t)> on_complete)
{
    // Wave 5 会实现真正的 HostLauncher
    // 这里先预留接口
    PostRequest(
        [this, plugin_path, on_complete]()
        {
            // TODO: 实现 HostLauncher
            if (on_complete)
            {
                on_complete(DAS_E_NO_IMPLEMENTATION, 0);
            }
        });
}
void IpcRunLoop::PostStopHost(uint16_t session_id)
{
    PostRequest(
        [this, session_id]()
        {
            // TODO: 实现 HostLauncher
            (void)session_id;
        });
}
DAS_CORE_IPC_NS_END
