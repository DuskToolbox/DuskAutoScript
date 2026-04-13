#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/_autogen/idl/ipc/IpcStubFactory.h>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>

#include <das/Core/IPC/DasReadOnlyStringStub.h>
#include <das/Core/IPC/DasVariantVectorByValueStub.h>
#include <das/Core/IPC/DefaultAsyncIpcTransport.h>
#include <das/Core/IPC/QueryInterfaceStub.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>

#include <das/Core/Logger/Logger.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

IpcRunLoop::IpcRunLoop(
    bool                             enable_heartbeat,
    IpcMessageQueue<InboundMessage>* inbound_queue,
    ProxyFactory&                    proxy_factory,
    RemoteObjectRegistry&            registry)
    : enable_heartbeat_(enable_heartbeat), inbound_queue_(inbound_queue),
      proxy_factory_(proxy_factory), registry_(registry)
{
    io_context_ = std::make_unique<boost::asio::io_context>();
    timeout_timer_ = std::make_unique<boost::asio::steady_timer>(*io_context_);

    connection_manager_ =
        std::make_unique<ConnectionManager>(1); // local_id = 1 for MainProcess

    // 注册所有 IPC stub handlers（代码生成器生成的 g_stub_factory）
    DasIpcStub::g_stub_factory.RegisterAll(*this);
    DAS_CORE_LOG_INFO("Registered all IPC stub handlers");

    // Register manual (non-IDL) stub handlers
    {
        static DasReadOnlyStringStub s_readonly_string_stub;
        RegisterHandler(
            HeaderFlags::NONE,
            DasReadOnlyStringStub::InterfaceId,
            &s_readonly_string_stub);
    }
    {
        static DasVariantVectorByValueStub s_variant_vector_stub;
        RegisterHandler(
            HeaderFlags::BUSINESS_EVENT,
            DasVariantVectorByValueStub::InterfaceId,
            &s_variant_vector_stub);
    }

    {
        static QueryInterfaceStub s_query_interface_stub;
        RegisterHandler(
            HeaderFlags::BUSINESS_CONTROL,
            static_cast<uint32_t>(IpcCommandType::QUERY_INTERFACE),
            &s_query_interface_stub);
    }
}

IpcRunLoop::~IpcRunLoop()
{
    RequestStop();

    if (connection_manager_)
    {
        connection_manager_->StopHeartbeatThread();
        connection_manager_.reset();
    }

    if (io_context_ && !io_context_->stopped())
    {
        io_context_->stop();
    }

    work_guard_.reset();
    timeout_timer_.reset();
    io_context_.reset();
}

void IpcRunLoop::RequestStop()
{
    if (!running_.load())
    {
        return;
    }

    running_.store(false);

    // 注意：IpcRunLoop 不再持有 async_transport_
    // Transport 的关闭由外部管理（HostLauncher 或 IpcContext）

    // 1. 重置 work guard，允许 io_context_->run() 返回
    work_guard_.reset();

    // 2. 取消所有 pending calls
    {
        std::vector<PendingCallCompletion> completions;
        {
            std::unique_lock<std::mutex> lock(pending_mutex_);
            for (auto& [call_key, state] : pending_calls_)
            {
                if (state.on_complete)
                {
                    completions.push_back(std::move(state.on_complete));
                }
            }
            pending_calls_.clear();
        }
        for (auto& cb : completions)
        {
            cb(DAS_E_IPC_CONNECTION_LOST, {}, uint16_t{0});
        }
    }

    // 3. 停止 io_context，强制所有异步操作取消
    // 这是必要的，因为接收协程可能阻塞在 co_await transport->ReceiveCoroutine()
    // 仅仅重置 work_guard 不足以让它退出
    if (io_context_)
    {
        io_context_->stop();
    }
}

void IpcRunLoop::RegisterHandler(
    uint8_t          header_flags,
    uint32_t         interface_id,
    IMessageHandler* handler)
{
    if (!handler)
    {
        return;
    }
    // 通过 DasPtr 构造（内部调用 AddRef）管理引用计数
    handlers_by_flags_[header_flags][interface_id] =
        DasPtr<IMessageHandler>(handler);
}

void IpcRunLoop::RegisterHandler(
    uint8_t                   header_flags,
    uint32_t                  interface_id,
    IAwaitableMessageHandler* handler)
{
    if (!handler)
    {
        return;
    }
    // 通过 DasPtr 构造（内部调用 AddRef）管理引用计数
    awaitable_handlers_[header_flags][interface_id] =
        DasPtr<IAwaitableMessageHandler>(handler);
}

IMessageHandler* IpcRunLoop::GetHandler(
    uint8_t  header_flags,
    uint32_t interface_id) const
{
    auto it = handlers_by_flags_.find(header_flags);
    if (it == handlers_by_flags_.end())
    {
        return nullptr;
    }

    auto jt = it->second.find(interface_id);
    if (jt != it->second.end())
    {
        return jt->second.Get();
    }

    return nullptr;
}

boost::asio::awaitable<void> IpcRunLoop::DispatchToHandlerCoroutine(
    const ValidatedIPCMessageHeader& header,
    const std::vector<uint8_t>&      body,
    DefaultAsyncIpcTransport&        transport)
{
    DAS_CORE_LOG_INFO(
        "DispatchToHandlerCoroutine: interface_id={}, call_id={}, header_flags={}, body_size={}",
        header.GetInterfaceId(),
        header.GetCallId(),
        header.GetHeaderFlags(),
        body.size());

    // 转发检查：如果 target_session_id 不是本地 session，转发到目标
    if (connection_manager_ && header.GetTargetSessionId() != 0
        && header.GetTargetSessionId() != local_session_id_)
    {
        DAS_CORE_LOG_DEBUG(
            "Forwarding message: target={}, source={}, type={}",
            header.GetTargetSessionId(),
            header.GetSourceSessionId(),
            static_cast<int>(header.GetMessageType()));

        // Route through send queue instead of direct ForwardMessage
        // to prevent concurrent async_write on the same pipe
        auto* fwd_transport =
            connection_manager_->GetTransport(header.GetTargetSessionId());
        if (fwd_transport)
        {
            std::vector<uint8_t> fwd_body(body.begin(), body.end());
            auto                 fwd_result = PostSendWithTransport(
                fwd_transport,
                header,
                std::move(fwd_body));
            if (fwd_result != DAS_S_OK)
            {
                DAS_CORE_LOG_ERROR(
                    "Forward failed: target={}, result={}",
                    header.GetTargetSessionId(),
                    fwd_result);
            }
        }
        else
        {
            DAS_CORE_LOG_ERROR(
                "Forward failed: no transport for target={}",
                header.GetTargetSessionId());
        }

        co_return;
    }

    // 处理 HEARTBEAT RESPONSE：收到心跳回复时更新时间戳
    if (header.GetInterfaceId()
        == static_cast<uint32_t>(
            HandshakeInterfaceId::HANDSHAKE_IFACE_HEARTBEAT))
    {
        if (header.GetMessageType() == MessageType::RESPONSE)
        {
            // 收到心跳回复，更新时间戳
            if (connection_manager_)
            {
                // V3: 使用 source_session_id
                connection_manager_->UpdateHeartbeatTimestamp(
                    header.GetSourceSessionId());
            }
            co_return;
        }
        // REQUEST 继续交给 handler 处理
    }

    // 使用 header_flags + interface_id 路由
    // 优先检查可等待 handler（协程版本，控制平面用）
    auto awaitable_it = awaitable_handlers_.find(header.GetHeaderFlags());
    if (awaitable_it != awaitable_handlers_.end())
    {
        auto& awaitable_map = awaitable_it->second;
        auto awaitable_handler_it = awaitable_map.find(header.GetInterfaceId());
        if (awaitable_handler_it != awaitable_map.end())
        {
            IAwaitableMessageHandler* awaitable_handler =
                awaitable_handler_it->second.Get();
            DAS_CORE_LOG_INFO(
                "DispatchToHandlerCoroutine: found awaitable handler for interface_id={}",
                header.GetInterfaceId());
            try
            {
                IpcResponseSender sender(transport, *this);
                StubContext       ctx{
                    proxy_factory_.GetObjectManager(),
                    registry_,
                    *this,
                    {},
                    proxy_factory_,
                    header};
                auto result = co_await awaitable_handler
                                  ->HandleMessage(header, body, sender, ctx);
                if (DAS::IsFailed(result))
                {
                    DAS_CORE_LOG_WARN(
                        "Awaitable handler returned error: {}",
                        result);
                }
                else
                {
                    DAS_CORE_LOG_INFO(
                        "DispatchToHandlerCoroutine: awaitable handler completed successfully");
                }
            }
            catch (const std::exception& e)
            {
                DAS_CORE_LOG_ERROR(
                    "DispatchToHandlerCoroutine: exception in awaitable HandleMessage: {}",
                    e.what());
            }
            co_return;
        }
    }

    // 同步 handler（业务 handler 用）
    IMessageHandler* handler =
        GetHandler(header.GetHeaderFlags(), header.GetInterfaceId());

    if (handler)
    {
        DAS_CORE_LOG_INFO(
            "DispatchToHandlerCoroutine: found handler for interface_id={}",
            header.GetInterfaceId());
        try
        {
            IpcResponseSender sender(transport, *this);
            StubContext       ctx{
                proxy_factory_.GetObjectManager(),
                registry_,
                *this,
                {},
                proxy_factory_,
                header};
            auto result = handler->HandleMessage(header, body, sender, ctx);
            if (DAS::IsFailed(result))
            {
                DAS_CORE_LOG_WARN("Handler returned error: {}", result);
            }
            else
            {
                DAS_CORE_LOG_INFO(
                    "DispatchToHandlerCoroutine: handler completed successfully");
            }
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_ERROR(
                "DispatchToHandlerCoroutine: exception in HandleMessage: {}",
                e.what());
        }
    }
    else
    {
        DAS_CORE_LOG_WARN(
            "No handler found for interface_id = {}",
            header.GetInterfaceId());
    }
}

std::pair<DasResult, CallKey> IpcRunLoop::PrepareSendRequestWithTransport(
    DefaultAsyncIpcTransport*        transport,
    const ValidatedIPCMessageHeader& request_header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    if (!transport)
    {
        return {
            DAS_E_INVALID_ARGUMENT,
            CallKey{.source_session_id = 0, .call_id = 0}};
    }

    uint16_t call_id = AllocateCallId();

    const IPCMessageHeader& raw = request_header.Raw();
    // V3: pending_call key 使用 target_session_id（对方进程的 session_id），
    // 这样响应返回时可以正确匹配
    CallKey call_key{
        .source_session_id = raw.target_session_id,
        .call_id = call_id};

    // V3: method_id 和 ObjectId 在 body 中携带，header 只保留 interface_id 和
    // session 路由信息
    auto validated_header = IPCMessageHeaderBuilder()
                                .SetMessageType(MessageType::REQUEST)
                                .SetInterfaceId(raw.interface_id)
                                .SetHeaderFlags(raw.header_flags)
                                .SetBodySize(raw.body_size)
                                .SetCallId(call_id)
                                .SetSourceSessionId(local_session_id_)
                                .SetTargetSessionId(raw.target_session_id)
                                .Build();

    {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_[call_key] = PendingCallState{
            .call_key = call_key,
            .deadline = std::chrono::steady_clock::time_point::max(),
            .on_complete = nullptr};
    }

    // 使用 co_spawn + use_future 调用协程发送
    try
    {
        auto future = boost::asio::co_spawn(
            *io_context_,
            transport->SendCoroutine(validated_header, body, body_size),
            boost::asio::use_future);

        DasResult send_result = future.get();

        if (send_result != DAS_S_OK)
        {
            std::unique_lock<std::mutex> lock(pending_mutex_);
            pending_calls_.erase(call_key);
            return {send_result, CallKey{.source_session_id = 0, .call_id = 0}};
        }

        return {send_result, call_key};
    }
    catch (const boost::system::system_error& e)
    {
        DAS_CORE_LOG_ERROR(
            "Send failed (system_error): {}",
            ToString(e.what()));
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_.erase(call_key);
        return {
            DAS_E_IPC_CONNECTION_LOST,
            CallKey{.source_session_id = 0, .call_id = 0}};
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR("Send failed: {}", ToString(e.what()));
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_.erase(call_key);
        return {
            DAS_E_IPC_CONNECTION_LOST,
            CallKey{.source_session_id = 0, .call_id = 0}};
    }
}

void IpcRunLoop::CompletePendingCall(
    CallKey              call_key,
    DasResult            result,
    std::vector<uint8_t> response,
    uint16_t             response_flags)
{
    DAS_CORE_LOG_INFO(
        "CompletePendingCall: source_session_id={}, call_id={}, result={}, response_size={}, flags=0x{:04X}",
        call_key.source_session_id,
        call_key.call_id,
        result,
        response.size(),
        response_flags);

    PendingCallCompletion on_complete;

    {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        auto                         it = pending_calls_.find(call_key);
        if (it == pending_calls_.end())
        {
            DAS_CORE_LOG_WARN(
                "CompletePendingCall: call_key not found in pending_calls");
            return;
        }

        it->second.response_flags = response_flags;
        on_complete = std::move(it->second.on_complete);
        pending_calls_.erase(it);
    }

    if (on_complete)
    {
        DAS_CORE_LOG_INFO(
            "CompletePendingCall: invoking callback for call_id={}",
            call_key.call_id);
        on_complete(result, std::move(response), response_flags);
    }
}

void IpcRunLoop::TickPendingSenders()
{
    auto now = std::chrono::steady_clock::now();

    std::vector<std::pair<CallKey, PendingCallCompletion>> expired;

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

    for (auto& [call_key, cb] : expired)
    {
        cb(DAS_E_IPC_TIMEOUT, {}, uint16_t{0});
    }

    // SHM stale block cleanup (every 60s when idle)
    if (pending_calls_.empty())
    {
        if (now - last_shm_cleanup_time_ > std::chrono::seconds(60))
        {
            last_shm_cleanup_time_ = now;
            if (connection_manager_)
            {
                connection_manager_->CleanupAllStaleBlocks();
            }
        }
    }
}

uint32_t IpcRunLoop::GetNearestDeadlineMs() const
{
    std::unique_lock<std::mutex> lock(pending_mutex_);

    if (pending_calls_.empty())
    {
        return 0;
    }

    auto now = std::chrono::steady_clock::now();
    auto nearest = std::chrono::steady_clock::time_point::max();

    for (const auto& [call_key, state] : pending_calls_)
    {
        (void)call_key;
        if (state.on_complete && state.deadline < nearest)
        {
            nearest = state.deadline;
        }
    }

    if (nearest == std::chrono::steady_clock::time_point::max())
    {
        return 0;
    }

    if (nearest <= now)
    {
        return 1;
    }

    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(nearest - now);
    return std::max(
        static_cast<uint32_t>(1),
        static_cast<uint32_t>(ms.count()));
}

void IpcRunLoop::RegisterPendingCompletion(
    CallKey                               call_key,
    std::chrono::steady_clock::time_point deadline,
    PendingCallCompletion                 on_complete)
{
    {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        auto                         it = pending_calls_.find(call_key);
        if (it != pending_calls_.end())
        {
            it->second.deadline = deadline;
            it->second.on_complete = std::move(on_complete);
            return;
        }
    }

    if (on_complete)
    {
        on_complete(DAS_E_IPC_TIMEOUT, {}, 0);
    }
}

void IpcRunLoop::RegisterPendingCall(CallKey call_key)
{
    std::unique_lock<std::mutex> lock(pending_mutex_);
    pending_calls_[call_key] = PendingCallState{
        .call_key = call_key,
        .deadline = std::chrono::steady_clock::time_point::max(),
        .on_complete = nullptr};
}

bool IpcRunLoop::IsRunning() const { return running_.load(); }

void IpcRunLoop::SetSessionId(uint16_t session_id)
{
    local_session_id_ = session_id;
}

DasResult IpcRunLoop::PostSend(
    const ValidatedIPCMessageHeader& header,
    std::vector<uint8_t>&&           body)
{
    if (!io_context_)
    {
        DAS_CORE_LOG_ERROR("PostSend: io_context_ is null");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    boost::asio::post(
        *io_context_,
        [this, header, body = std::move(body)]() mutable
        {
            // Get launcher first to extend transport lifetime during async
            // send. DasPtr prevents HostLauncher destruction while coroutine
            // is running (heartbeat thread may erase from host_launchers_).
            auto launcher =
                connection_manager_->GetLauncher(header.GetTargetSessionId());

            DefaultAsyncIpcTransport* transport = nullptr;
            if (launcher)
            {
                transport = launcher->GetTransport();
            }

            // Fallback: direct transport (Host mode, lifetime managed by
            // IpcContext outliving the io_context event loop)
            if (!transport)
            {
                transport = connection_manager_->GetTransport(
                    header.GetTargetSessionId());
            }

            if (!transport)
            {
                DAS_CORE_LOG_ERROR(
                    "PostSend: no transport for session = {}",
                    header.GetTargetSessionId());
                NotifySendFailure(header, DAS_E_IPC_NO_CONNECTIONS);
                return;
            }

            // launcher (possibly null for Host mode) is captured to keep
            // HostLauncher and its transport alive for the coroutine duration
            boost::asio::co_spawn(
                *io_context_,
                [this,
                 transport,
                 launcher,
                 header,
                 body =
                     std::move(body)]() mutable -> boost::asio::awaitable<void>
                {
                    try
                    {
                        auto result = co_await transport->SendCoroutine(
                            header,
                            body.data(),
                            body.size());
                        if (result != DAS_S_OK)
                        {
                            NotifySendFailure(header, result);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        DAS_CORE_LOG_ERROR(
                            "PostSend: exception: {}",
                            ToString(e.what()));
                        NotifySendFailure(header, DAS_E_IPC_SEND_FAILED);
                    }
                },
                boost::asio::detached);
        });

    return DAS_S_OK;
}

DasResult IpcRunLoop::PostSendWithTransport(
    DefaultAsyncIpcTransport*        transport,
    const ValidatedIPCMessageHeader& header,
    std::vector<uint8_t>&&           body)
{
    if (!io_context_)
    {
        DAS_CORE_LOG_ERROR("PostSendWithTransport: io_context_ is null");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    if (!transport)
    {
        DAS_CORE_LOG_ERROR("PostSendWithTransport: transport is null");
        return DAS_E_IPC_INVALID_ARGUMENT;
    }

    // Get launcher to extend transport lifetime during async send.
    // DasPtr prevents HostLauncher destruction while coroutine is running
    // (heartbeat thread may erase from host_launchers_).
    // May be null for Host mode (direct transport, lifetime managed by
    // IpcContext).
    auto launcher =
        connection_manager_
            ? connection_manager_->GetLauncher(header.GetTargetSessionId())
            : nullptr;

    // launcher is captured (possibly null) to keep HostLauncher and its
    // transport alive for the coroutine duration
    boost::asio::co_spawn(
        *io_context_,
        [this, transport, launcher, header, body = std::move(body)]() mutable
            -> boost::asio::awaitable<void>
        {
            try
            {
                auto result = co_await transport->SendCoroutine(
                    header,
                    body.data(),
                    body.size());
                if (result != DAS_S_OK)
                {
                    NotifySendFailure(header, result);
                }
            }
            catch (const std::exception& e)
            {
                DAS_CORE_LOG_ERROR(
                    "PostSendWithTransport: exception: {}",
                    ToString(e.what()));
                NotifySendFailure(header, DAS_E_IPC_SEND_FAILED);
            }
        },
        boost::asio::detached);

    return DAS_S_OK;
}

void IpcRunLoop::NotifySendFailure(
    const ValidatedIPCMessageHeader& header,
    DasResult                        error_code)
{
    // 构造失败 RESPONSE，推入 inbound_queue_
    // BusinessThread::Run() → RedispatchMessage 会发现它是 RESPONSE
    // → 调用 CompletePendingCall，同时覆盖两条等待路径：
    //   - BusinessThread 路径：PumpUntilResponse 从队列中泵出
    //   - 外部线程路径：CompletePendingCall 触发 on_complete 回调
    //
    // 必须使用 IPCMessageHeaderBuilder 构造 ValidatedIPCMessageHeader，
    // 因为 ValidatedIPCMessageHeader(IPCMessageHeader&) 是 private 构造函数，
    // IpcRunLoop 不是友元类。IPCMessageHeaderBuilder 是友元，可正常调用
    // Build()。 IpcMessageHeaderBuilder.h 已在文件头部 include（第 11 行）。
    if (inbound_queue_)
    {
        // RESPONSE 的 source/target 必须与 REQUEST 互换（与 IStubBase 一致），
        // 否则 CallKey 匹配不到 pending_calls_ 中的条目
        auto fail_header = IPCMessageHeaderBuilder()
                               .SetCallId(header.GetCallId())
                               .SetSourceSessionId(header.GetTargetSessionId())
                               .SetTargetSessionId(header.GetSourceSessionId())
                               .SetErrorCode(static_cast<int32_t>(error_code))
                               .Build();

        InboundMessage msg;
        msg.header = std::move(fail_header);
        // body 为空 — 错误码已在 header.error_code 中
        auto push_result = inbound_queue_->Push(std::move(msg));
        if (push_result != DAS_S_OK)
        {
            DAS_CORE_LOG_ERROR(
                "NotifySendFailure: failed to push to inbound_queue_, "
                "result = {}",
                push_result);
        }
    }
    else
    {
        DAS_CORE_LOG_ERROR(
            "NotifySendFailure: inbound_queue_ is null, cannot notify "
            "failure for call_id = {}, error_code = 0x{:08X}",
            header.GetCallId(),
            error_code);
    }
}

void IpcRunLoop::ScheduleTimeoutCheck()
{
    if (!io_context_ || !timeout_timer_)
    {
        return;
    }

    uint32_t timeout_ms = GetNearestDeadlineMs();
    if (timeout_ms == 0)
    {
        // 没有超时任务，不需要调度定时器
        // 当有新的 pending call 时会重新调度
        return;
    }

    timeout_timer_->expires_after(std::chrono::milliseconds(timeout_ms));
    timeout_timer_->async_wait(
        [this](const boost::system::error_code& ec)
        {
            if (!running_.load() || ec)
            {
                return;
            }

            TickPendingSenders();

            // 重新调度下一次检查
            ScheduleTimeoutCheck();
        });
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

    // 创建 work guard 保持 io_context 运行
    // 确保 Run() 阻塞直到 RequestStop() 被调用
    work_guard_.emplace(io_context_->get_executor());

    // 启动心跳检测线程（MainProcess 模式，且启用心跳时）
    if (connection_manager_ && enable_heartbeat_)
    {
        connection_manager_->StartHeartbeatThread();
    }

    // 启动超时检查定时器
    ScheduleTimeoutCheck();

    // 阻塞运行事件循环
    io_context_->run();

    return exit_code_.load();
}

DasResult IpcRunLoop::RegisterHostLauncher(DasPtr<HostLauncher> launcher)
{
    if (!launcher)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    uint16_t session_id = launcher->GetSessionId();

    auto* transport = launcher->GetTransport();
    if (!transport)
    {
        DAS_CORE_LOG_ERROR(
            "HostLauncher has no Transport: session_id={}",
            session_id);
        return DAS_E_IPC_NO_CONNECTIONS;
    }

    // Register with ConnectionManager
    auto* conn_mgr = GetConnectionManager();
    if (!conn_mgr)
    {
        DAS_CORE_LOG_ERROR("ConnectionManager not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    DasResult result =
        conn_mgr->RegisterHostLauncher(session_id, std::move(launcher));
    if (result != DAS_S_OK)
    {
        return result;
    }

    // Start async receive for this Transport
    // 注意：从 ConnectionManager 获取 DasPtr 副本，协程会捕获它来保持 transport
    // 存活
    StartAsyncReceiveForTransport(
        session_id,
        conn_mgr->GetLauncher(session_id));

    DAS_CORE_LOG_INFO("HostLauncher registered: session_id={}", session_id);
    return DAS_S_OK;
}

void IpcRunLoop::StartAsyncReceiveForTransport(
    uint16_t             session_id,
    DasPtr<HostLauncher> launcher)
{
    if (!launcher)
    {
        DAS_CORE_LOG_WARN(
            "Launcher is null, skipping receive loop: session_id={}",
            session_id);
        return;
    }

    auto* transport = launcher->GetTransport();
    if (!transport || !transport->IsConnected())
    {
        DAS_CORE_LOG_WARN(
            "Transport not connected, skipping receive loop: session_id={}",
            session_id);
        return;
    }

    // Spawn receive coroutine
    // 注意：捕获 launcher 的 DasPtr 来保持 transport 存活
    // 当协程运行时，launcher 及其 transport 不会被销毁
    boost::asio::co_spawn(
        *io_context_,
        [this, session_id, launcher]() -> boost::asio::awaitable<void>
        {
            std::optional<int> retry_error_code;
            while (running_.load())
            {
                retry_error_code.reset();

                try
                {
                    // 从 launcher 获取 transport（安全，因为 launcher 被 DasPtr
                    // 持有）
                    auto* transport = launcher->GetTransport();
                    if (!transport)
                    {
                        DAS_CORE_LOG_DEBUG(
                            "Transport became null, exiting: session_id={}",
                            session_id);
                        co_return;
                    }

                    // Use the transport's coroutine interface
                    auto result = co_await transport->ReceiveCoroutine();

                    if (!running_.load())
                    {
                        co_return;
                    }

                    if (result.index() == 0)
                    {
                        // Error case
                        DasResult error_code = std::get<0>(result);
                        if (error_code != DAS_S_OK)
                        {
                            DAS_CORE_LOG_ERROR(
                                "Receive failed with error: session_id = {}, error = {}",
                                session_id,
                                error_code);
                        }
                        co_return;
                    }

                    // Successfully received message
                    auto&& [header, body] = std::get<1>(result);

                    DAS_CORE_LOG_INFO(
                        "Client ReceiveLoop: session_id={}, msg_type={}, interface_id={}, call_id={}, header_flags={}",
                        session_id,
                        static_cast<int>(header.Raw().message_type),
                        header.Raw().interface_id,
                        header.Raw().call_id,
                        static_cast<int>(header.Raw().header_flags));

                    // IO 线程消息分流：按 header_flags 决定处理方式
                    if ((header.GetHeaderFlags() & HeaderFlags::CONTROL_PLANE)
                        != 0)
                    {
                        // 控制平面消息：在 IO 线程直接处理
                        if (header.GetMessageType() == MessageType::RESPONSE)
                        {
                            // V3: 使用 (source_session_id, call_id) 匹配响应
                            CallKey call_key{
                                header.GetSourceSessionId(),
                                header.GetCallId()};
                            DAS_CORE_LOG_INFO(
                                "Client: RESPONSE (control plane) received for source_session_id={}, call_id={}",
                                call_key.source_session_id,
                                call_key.call_id);
                            CompletePendingCall(
                                call_key,
                                DAS_S_OK,
                                std::move(body),
                                header.GetFlags());
                        }
                        else
                        {
                            // REQUEST/EVENT: 在 IO 线程处理（控制平面 handler）
                            co_await DispatchToHandlerCoroutine(
                                header,
                                body,
                                *transport);
                        }
                    }
                    else
                    {
                        // 业务消息转发：跨 Host 消息需要转发到目标
                        if (connection_manager_
                            && header.GetTargetSessionId() != local_session_id_)
                        {
                            auto* fwd_transport =
                                connection_manager_->GetTransport(
                                    header.GetTargetSessionId());
                            if (fwd_transport)
                            {
                                DAS_CORE_LOG_INFO(
                                    "Forwarding business message: "
                                    "target={}, source={}, type={}, "
                                    "interface_id={}",
                                    header.GetTargetSessionId(),
                                    header.GetSourceSessionId(),
                                    static_cast<int>(header.GetMessageType()),
                                    header.GetInterfaceId());

                                // Route through send queue to prevent
                                // concurrent async_write
                                std::vector<uint8_t> fwd_body(
                                    body.begin(),
                                    body.end());
                                auto fwd_result = PostSendWithTransport(
                                    fwd_transport,
                                    header,
                                    std::move(fwd_body));

                                if (fwd_result != DAS_S_OK)
                                {
                                    DAS_CORE_LOG_ERROR(
                                        "Business message forward failed: "
                                        "target={}, result={}",
                                        header.GetTargetSessionId(),
                                        fwd_result);
                                }
                            }
                            else
                            {
                                DAS_CORE_LOG_ERROR(
                                    "Business message forward failed: "
                                    "no transport for target={}",
                                    header.GetTargetSessionId());
                            }
                            co_return;
                        }

                        // 业务 RESPONSE 快速路径：直接在 IO 线程完成 pending
                        // call，避免经过 BusinessThread 的额外调度开销
                        if (header.GetMessageType() == MessageType::RESPONSE)
                        {
                            CallKey call_key{
                                header.GetSourceSessionId(),
                                header.GetCallId()};
                            std::unique_lock<std::mutex> lock(pending_mutex_);
                            auto it = pending_calls_.find(call_key);
                            if (it != pending_calls_.end()
                                && it->second.on_complete)
                            {
                                lock.unlock();
                                DAS_CORE_LOG_INFO(
                                    "Business RESPONSE fast path: "
                                    "source_session_id={}, call_id={}",
                                    call_key.source_session_id,
                                    call_key.call_id);
                                CompletePendingCall(
                                    call_key,
                                    DAS_S_OK,
                                    std::move(body),
                                    header.GetFlags());
                                // 已在 IO 线程完成，跳过 inbound_queue_
                            }
                            else
                            {
                                lock.unlock();
                                // 无匹配的 pending call，仍投递到
                                // inbound_queue_ 供 PumpUntilResponse 消费
                                InboundMessage msg;
                                msg.header = header;
                                msg.body = std::move(body);
                                auto push_result =
                                    inbound_queue_->Push(std::move(msg));
                                if (push_result != DAS_S_OK)
                                {
                                    DAS_CORE_LOG_ERROR(
                                        "Client ReceiveLoop: failed to push to inbound_queue: session_id={}, interface_id={}, result={}",
                                        session_id,
                                        header.GetInterfaceId(),
                                        push_result);
                                }
                            }
                        }
                        else
                        {
                            // REQUEST/EVENT: 投递到 inbound_queue
                            if (!inbound_queue_)
                            {
                                DAS_CORE_LOG_WARN(
                                    "Client ReceiveLoop: inbound_queue_ is null, dropping message: session_id={}, interface_id={}",
                                    session_id,
                                    header.GetInterfaceId());
                            }
                            else
                            {
                                InboundMessage msg;
                                msg.header = header;
                                msg.body = std::move(body);
                                auto push_result =
                                    inbound_queue_->Push(std::move(msg));
                                if (push_result != DAS_S_OK)
                                {
                                    DAS_CORE_LOG_ERROR(
                                        "Client ReceiveLoop: failed to push to inbound_queue: session_id={}, interface_id={}, result={}",
                                        session_id,
                                        header.GetInterfaceId(),
                                        push_result);
                                }
                            }
                        }
                    }
                }
                catch (const boost::system::system_error& e)
                {
                    // operation_aborted: normal stop
                    if (e.code() == boost::asio::error::operation_aborted)
                    {
                        DAS_CORE_LOG_DEBUG(
                            "Receive loop stopped - operation aborted: session_id={}",
                            session_id);
                        co_return;
                    }
                    // EOF: pipe closed
                    else if (e.code() == boost::asio::error::eof)
                    {
                        DAS_CORE_LOG_DEBUG(
                            "Receive loop stopped - EOF reached: session_id={}",
                            session_id);
                        co_return;
                    }
                    // ERROR_NO_DATA (536): waiting for pipe connection
                    else if (e.code().value() == 536)
                    {
                        // Store error code for retry outside catch block
                        retry_error_code = e.code().value();
                    }
                    else
                    {
                        DAS_CORE_LOG_ERROR(
                            "Receive failed with system error: session_id={}, error={}",
                            session_id,
                            ToString(e.what()));
                        co_return;
                    }
                }
                catch (const std::exception& e)
                {
                    DAS_CORE_LOG_ERROR(
                        "Receive failed with exception: session_id={}, error={}",
                        session_id,
                        ToString(e.what()));
                    co_return;
                }

                // Handle retry outside catch block (co_await not allowed in
                // catch)
                if (retry_error_code.has_value())
                {
                    DAS_CORE_LOG_DEBUG(
                        "Waiting for client to connect: session_id={}",
                        session_id);
                    auto timer = boost::asio::steady_timer(*io_context_);
                    timer.expires_after(std::chrono::milliseconds(100));
                    co_await timer.async_wait(boost::asio::use_awaitable);
                    // continue loop to retry receive
                }
            }
        },
        boost::asio::detached);
}

DAS_CORE_IPC_NS_END
