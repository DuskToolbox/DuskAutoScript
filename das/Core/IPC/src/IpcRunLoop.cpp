#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IHostConnection.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/InternalCallbackHandler.h>
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
#include <das/Core/IPC/QueryInterfaceStub.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>

#include <das/Core/Logger/Logger.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

namespace
{
    bool IsControlPlaneMessage(const ValidatedIPCMessageHeader& header) noexcept
    {
        return (header.GetHeaderFlags() & HeaderFlags::CONTROL_PLANE) != 0;
    }

    bool IsHeartbeatMessage(const ValidatedIPCMessageHeader& header) noexcept
    {
        return header.GetInterfaceId()
               == static_cast<uint32_t>(
                   HandshakeInterfaceId::HandshakeIfaceHeartbeat);
    }

    DasResult GetResponseResult(
        const ValidatedIPCMessageHeader& header) noexcept
    {
        if (header.HasError())
        {
            return static_cast<DasResult>(header.GetErrorCode());
        }

        return DAS_S_OK;
    }
} // namespace

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
            static_cast<uint32_t>(IpcCommandType::QueryInterface),
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
    // 无论 running_ 状态如何，先标记 io_context_ 已停止。
    // 这处理 RequestStop() 与 Run() 启动并发执行、且观察到 running_==false
    // 的竞态： 若不在此处 stop，后续 Run() 会因 work_guard_ 仍存在而永久阻塞在
    // io_context_->run() 上。io_context::stop() 线程安全且幂等。
    if (io_context_)
    {
        io_context_->stop();
    }

    if (!running_.load())
    {
        return;
    }

    running_.store(false);

    // 1. 重置 work guard，允许 io_context_->run() 返回
    work_guard_.reset();

    // 2. 取消所有 pending calls
    {
        std::vector<PendingCallCompletion> completions;
        {
            const std::unique_lock<std::mutex> lock(pending_mutex_);
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

    // io_context_->stop() 已在函数入口执行，接收协程的 co_await
    // transport->ReceiveCoroutine() 会因 operation_aborted 退出。
}

size_t IpcRunLoop::FailPendingCallsBySession(
    uint16_t  session_id,
    DasResult error)
{
    // host 断连/崩溃：该 session 的 pending call 永远收不到回包，主动 fail
    // 回传 error 让调用方（HTTP handler / sync_wait）解除阻塞。复用 RequestStop
    // 的 fail 模式（锁内收集 + 锁外回调，避免持锁调用户回调死锁），按
    // source_session_id 过滤，不影响其他 host 的 pending。
    std::vector<PendingCallCompletion> completions;
    {
        const std::unique_lock<std::mutex> lock(pending_mutex_);
        for (auto it = pending_calls_.begin(); it != pending_calls_.end();)
        {
            if (it->first.source_session_id == session_id)
            {
                if (it->second.on_complete)
                {
                    completions.push_back(std::move(it->second.on_complete));
                }
                it = pending_calls_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    for (auto& cb : completions)
    {
        cb(error, {}, uint16_t{0});
    }
    if (!completions.empty())
    {
        DAS_CORE_LOG_WARN(
            "Failed {} pending call(s) for disconnected session: session_id={}",
            completions.size(),
            session_id);
    }
    return completions.size();
}

void IpcRunLoop::SetOnHostSessionDisconnected(
    HostSessionDisconnectedCallback callback)
{
    on_host_session_disconnected_ = std::move(callback);
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
    AnyTransport&                    transport)
{
    (void)transport;
    IpcResponseSender sender(*this);
    co_await DispatchToHandlerWithSender(header, body, sender);
}

boost::asio::awaitable<void> IpcRunLoop::DispatchToHandlerCoroutine(
    const ValidatedIPCMessageHeader& header,
    const std::vector<uint8_t>&      body,
    DasPtr<IHostConnection>          connection)
{
    IpcResponseSender sender(
        IpcResponseSender::HostConnectionRoute{this, std::move(connection)});
    co_await DispatchToHandlerWithSender(header, body, sender);
}

boost::asio::awaitable<void> IpcRunLoop::DispatchToHandlerWithSender(
    const ValidatedIPCMessageHeader& header,
    const std::vector<uint8_t>&      body,
    IpcResponseSender&               sender)
{
    DAS_CORE_LOG_INFO(
        "Dispatching handler: interface_id = {}, call_id = {}, "
        "header_flags = {}, body_size = {}",
        header.GetInterfaceId(),
        header.GetCallId(),
        header.GetHeaderFlags(),
        body.size());

    // CONTROL_PLANE dispatch: use IO-safe ControlPlaneContext.
    // This path runs on the IO coroutine domain and must not access
    // BusinessThread-owned DOM/Registry/proxy state.
    // Priority: awaitable handler (IAwaitableMessageHandler +
    // ControlPlaneContext) over sync handler (IMessageHandler + StubContext).

    // 1. Check for awaitable handler (CONTROL_PLANE coroutine handler)
    auto awaitable_it = awaitable_handlers_.find(header.GetHeaderFlags());
    if (awaitable_it != awaitable_handlers_.end())
    {
        auto& awaitable_map = awaitable_it->second;
        auto awaitable_handler_it = awaitable_map.find(header.GetInterfaceId());
        if (awaitable_handler_it != awaitable_map.end())
        {
            IAwaitableMessageHandler* awaitable_handler =
                awaitable_handler_it->second.Get();
            try
            {
                ControlPlaneContext ctx{*this, header};
                auto result = co_await awaitable_handler
                                  ->HandleMessage(header, body, sender, ctx);
                if (DAS::IsFailed(result))
                {
                    DAS_CORE_LOG_WARN(
                        "Awaitable handler returned error: {}",
                        result);
                }
            }
            catch (const std::exception& e)
            {
                DAS_CORE_LOG_ERROR(
                    "Awaitable handler threw exception: {}",
                    e.what());
            }
            co_return;
        }
    }

    // 2. Sync handler via IMessageHandler + StubContext is NOT permitted on
    //    the CONTROL_PLANE IO path. StubContext contains BusinessThread-owned
    //    DOM/Registry/proxy references that must not be accessed from the IO
    //    thread. All CONTROL_PLANE handlers must be registered as
    //    IAwaitableMessageHandler. Business handlers (BUSINESS_CONTROL /
    //    BUSINESS_EVENT) continue through the inbound_queue_ ->
    //    BusinessThread::ProcessInboundMessage path where StubContext is
    //    safely constructed on the BusinessThread.
    const IMessageHandler* handler =
        GetHandler(header.GetHeaderFlags(), header.GetInterfaceId());

    if (handler)
    {
        DAS_CORE_LOG_ERROR(
            "Sync IMessageHandler registered for CONTROL_PLANE "
            "interface_id = 0x{:08X}; "
            "Use IAwaitableMessageHandler instead.",
            header.GetInterfaceId());
    }
    else
    {
        DAS_CORE_LOG_WARN(
            "No handler found for interface_id = 0x{:08X}",
            header.GetInterfaceId());
    }
}

boost::asio::awaitable<void> IpcRunLoop::RouteIncomingMessage(
    const ValidatedIPCMessageHeader& header,
    std::vector<uint8_t>             body)
{
    co_await RouteIncomingMessage(header, std::move(body), {});
}

boost::asio::awaitable<void> IpcRunLoop::RouteIncomingMessage(
    const ValidatedIPCMessageHeader& header,
    std::vector<uint8_t>             body,
    DasPtr<IHostConnection>          connection)
{
    // 捎带扩展段接收侧剥离（IO 协程域即时提取，分发/转发判断之前）。
    // 置位 RESERVED_AS_SIZE 时 body = [扩展段][正常 body]，扩展段 = 若干
    // 完整内嵌帧（32B header + body 连续拼接，body_size 自描述循环剥取）。
    // 转发兼容（D-12/T-104.2-17）：转发是不透明字节搬运——在途帧整帧
    // 原样透传、由终点递归剥离，以下判定与下方既有转发分支的条件保持
    // 一致（控制平面消息永远本地消费，不会进转发分支）。
    if ((header.GetFlags() & MessageFlags::RESERVED_AS_SIZE) != 0)
    {
        const bool is_transit_frame =
            !IsControlPlaneMessage(header) && connection_manager_
            && header.GetTargetSessionId() != 0
            && header.GetTargetSessionId() != local_session_id_;

        if (!is_transit_frame)
        {
            const auto section_result =
                IpcHeaderValidator::ValidateExtensionSection(
                    header.GetFlags(),
                    header.Raw().reserved,
                    static_cast<uint32_t>(body.size()),
                    header.GetHeaderFlags());
            if (!section_result.IsOk())
            {
                DAS_CORE_LOG_WARN(
                    "Extension section rejected, frame dropped: error = {}, "
                    "flags = {}, reserved = {}, body_size = {}",
                    section_result.GetErrorName(),
                    header.GetFlags(),
                    header.Raw().reserved,
                    header.GetBodySize());
                co_return;
            }

            // 剥离循环不变量：偏移单调推进，以 section_size 不超过可用
            // body 与内嵌 body_size 自描述为界——杜绝死循环/越界读
            const uint32_t section_size = header.Raw().reserved;
            size_t         offset = 0;
            while (offset < section_size)
            {
                const size_t remaining_section =
                    static_cast<size_t>(section_size) - offset;
                if (remaining_section < sizeof(IPCMessageHeader))
                {
                    DAS_CORE_LOG_WARN(
                        "Extension section truncated at header boundary, "
                        "host frame dropped: offset = {}, section_size = {}",
                        offset,
                        section_size);
                    co_return;
                }

                const uint8_t*         embedded_frame = body.data() + offset;
                HeaderValidationResult embedded_error;
                const auto             embedded_header =
                    ValidatedIPCMessageHeader::Deserialize(
                        embedded_frame,
                        sizeof(IPCMessageHeader),
                        &embedded_error);
                if (!embedded_header)
                {
                    DAS_CORE_LOG_WARN(
                        "Embedded frame header invalid, host frame dropped: "
                        "error = {}, offset = {}",
                        embedded_error.GetErrorName(),
                        offset);
                    co_return;
                }

                // 深度=1 硬约束：内嵌帧禁止再置 RESERVED_AS_SIZE
                const auto depth_result =
                    IpcHeaderValidator::ValidateExtensionDepth(
                        embedded_header->GetFlags());
                if (!depth_result.IsOk())
                {
                    DAS_CORE_LOG_WARN(
                        "Nested extension section rejected, host frame "
                        "dropped: error = {}, offset = {}",
                        depth_result.GetErrorName(),
                        offset);
                    co_return;
                }

                const size_t embedded_frame_size =
                    sizeof(IPCMessageHeader) + embedded_header->GetBodySize();
                if (embedded_frame_size > remaining_section)
                {
                    DAS_CORE_LOG_WARN(
                        "Embedded frame overruns extension section, host "
                        "frame dropped: declared = {}, remaining = {}",
                        embedded_frame_size,
                        remaining_section);
                    co_return;
                }

                std::vector<uint8_t> embedded_body(
                    embedded_frame + sizeof(IPCMessageHeader),
                    embedded_frame + embedded_frame_size);
                // 递归走同一入口：校验/分流/转发/日志全复用
                co_await RouteIncomingMessage(
                    *embedded_header,
                    std::move(embedded_body),
                    connection);
                offset += embedded_frame_size;
            }

            // 剩余 body 作为宿主正常 body：剥掉扩展段标记后经既有
            // Deserialize 路径重建 header，走既有路由
            IPCMessageHeader host_raw = header.Raw();
            host_raw.flags = static_cast<uint16_t>(
                host_raw.flags & ~MessageFlags::RESERVED_AS_SIZE);
            host_raw.reserved = 0;
            host_raw.body_size =
                static_cast<uint32_t>(body.size() - section_size);
            const auto host_header = ValidatedIPCMessageHeader::Deserialize(
                &host_raw,
                sizeof(host_raw));
            if (!host_header)
            {
                DAS_CORE_LOG_WARN(
                    "Host header rebuild failed after extension strip, "
                    "frame dropped");
                co_return;
            }

            std::vector<uint8_t> host_body(
                body.begin() + section_size,
                body.end());
            co_await RouteIncomingMessage(
                *host_header,
                std::move(host_body),
                connection);
            co_return;
        }
    }

    if (IsControlPlaneMessage(header))
    {
        if (header.GetMessageType() == MessageType::Response)
        {
            if (IsHeartbeatMessage(header) && connection_manager_)
            {
                connection_manager_->UpdateHeartbeatTimestamp(
                    header.GetSourceSessionId());
            }

            const CallKey call_key{
                header.GetSourceSessionId(),
                header.GetCallId()};
            static_cast<void>(TryCompletePendingCallbackOnly(
                call_key,
                GetResponseResult(header),
                body,
                header.GetFlags()));
            co_return;
        }

        if (connection)
        {
            co_await DispatchToHandlerCoroutine(
                header,
                body,
                std::move(connection));
        }
        else
        {
            IpcResponseSender sender(*this);
            co_await DispatchToHandlerWithSender(header, body, sender);
        }
        co_return;
    }

    if (connection_manager_ && header.GetTargetSessionId() != 0
        && header.GetTargetSessionId() != local_session_id_)
    {
        DAS_CORE_LOG_DEBUG(
            "Forwarding message: target = {}, source = {}, type = {}",
            header.GetTargetSessionId(),
            header.GetSourceSessionId(),
            static_cast<int>(header.GetMessageType()));

        DasPtr<IHostConnection> target_connection =
            connection_manager_->FindManagedHostConnection(
                header.GetTargetSessionId());
        DasResult fwd_result = DAS_E_IPC_OBJECT_NOT_FOUND;
        if (target_connection)
        {
            fwd_result = PostSendWithTransport(
                std::move(target_connection),
                header,
                std::move(body));
        }
        else
        {
            fwd_result = PostSend(header, std::move(body));
        }

        if (fwd_result != DAS_S_OK)
        {
            DAS_CORE_LOG_ERROR(
                "Forward failed: target = {}, result = {}",
                header.GetTargetSessionId(),
                fwd_result);
        }

        co_return;
    }

    if (header.GetMessageType() == MessageType::Response)
    {
        const CallKey call_key{header.GetSourceSessionId(), header.GetCallId()};
        if (TryCompletePendingCallbackOnly(
                call_key,
                GetResponseResult(header),
                body,
                header.GetFlags()))
        {
            co_return;
        }
    }

    if (!inbound_queue_)
    {
        DAS_CORE_LOG_ERROR(
            "Inbound queue is not initialized, dropping message: "
            "source = {}, target = {}, call_id = {}",
            header.GetSourceSessionId(),
            header.GetTargetSessionId(),
            header.GetCallId());
        co_return;
    }

    InboundMessage msg;
    msg.header = header;
    msg.body = std::move(body);
    auto push_result = inbound_queue_->Push(std::move(msg));
    if (push_result != DAS_S_OK)
    {
        DAS_CORE_LOG_ERROR(
            "Failed to push inbound message: source = {}, target = {}, "
            "result = {}",
            header.GetSourceSessionId(),
            header.GetTargetSessionId(),
            push_result);
    }
}

std::pair<ValidatedIPCMessageHeader, CallKey>
IpcRunLoop::PrepareAsyncSendHeader(
    const ValidatedIPCMessageHeader& request_header)
{
    const uint16_t          call_id = AllocateCallId();
    const IPCMessageHeader& raw = request_header.Raw();

    const CallKey call_key{
        .source_session_id = raw.target_session_id,
        .call_id = call_id};

    auto validated_header = IPCMessageHeaderBuilder()
                                .SetMessageType(MessageType::Request)
                                .SetInterfaceId(raw.interface_id)
                                .SetHeaderFlags(raw.header_flags)
                                .SetBodySize(raw.body_size)
                                .SetCallId(call_id)
                                .SetSourceSessionId(local_session_id_)
                                .SetTargetSessionId(raw.target_session_id)
                                .Build();

    return {validated_header, call_key};
}

std::pair<DasResult, CallKey> IpcRunLoop::PrepareSendRequestWithTransport(
    AnyTransport&                    transport,
    const ValidatedIPCMessageHeader& request_header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    const uint16_t call_id = AllocateCallId();

    const IPCMessageHeader& raw = request_header.Raw();
    // V3: pending_call key 使用 target_session_id（对方进程的 session_id），
    // 这样响应返回时可以正确匹配
    const CallKey call_key{
        .source_session_id = raw.target_session_id,
        .call_id = call_id};

    // V3: method_id 和 ObjectId 在 body 中携带，header 只保留 interface_id 和
    // session 路由信息
    auto validated_header = IPCMessageHeaderBuilder()
                                .SetMessageType(MessageType::Request)
                                .SetInterfaceId(raw.interface_id)
                                .SetHeaderFlags(raw.header_flags)
                                .SetBodySize(raw.body_size)
                                .SetCallId(call_id)
                                .SetSourceSessionId(local_session_id_)
                                .SetTargetSessionId(raw.target_session_id)
                                .Build();

    {
        const std::unique_lock<std::mutex> lock(pending_mutex_);
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
            transport.SendCoroutine(validated_header, body, body_size),
            boost::asio::use_future);

        const DasResult send_result = future.get();

        if (send_result != DAS_S_OK)
        {
            const std::unique_lock<std::mutex> lock(pending_mutex_);
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
        const std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_.erase(call_key);
        return {
            DAS_E_IPC_CONNECTION_LOST,
            CallKey{.source_session_id = 0, .call_id = 0}};
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR("Send failed: {}", ToString(e.what()));
        const std::unique_lock<std::mutex> lock(pending_mutex_);
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
        "Completing pending call: source_session_id = {}, call_id = {}, "
        "result = {}, response_size = {}, flags = 0x{:04X}",
        call_key.source_session_id,
        call_key.call_id,
        result,
        response.size(),
        response_flags);

    if (!TryCompletePendingCallbackOnly(
            call_key,
            result,
            response,
            response_flags))
    {
        DAS_CORE_LOG_WARN("Pending callback not found for call_key");
    }
}

bool IpcRunLoop::TryCompletePendingCallbackOnly(
    CallKey               call_key,
    DasResult             result,
    std::vector<uint8_t>& response,
    uint16_t              response_flags)
{
    PendingCallCompletion on_complete;

    {
        const std::unique_lock<std::mutex> lock(pending_mutex_);
        auto                               it = pending_calls_.find(call_key);
        if (it == pending_calls_.end())
        {
            return false;
        }

        if (!it->second.on_complete)
        {
            return false;
        }

        it->second.response_flags = response_flags;
        on_complete = std::move(it->second.on_complete);
        pending_calls_.erase(it);
    }

    if (on_complete)
    {
        DAS_CORE_LOG_INFO(
            "Invoking pending callback for call_id = {}",
            call_key.call_id);
        on_complete(result, std::move(response), response_flags);
    }

    return true;
}

void IpcRunLoop::TickPendingSenders()
{
    auto now = std::chrono::steady_clock::now();

    std::vector<std::pair<CallKey, PendingCallCompletion>> expired;

    bool calls_empty = false;
    {
        const std::unique_lock<std::mutex> lock(pending_mutex_);
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
        calls_empty = pending_calls_.empty();
    }

    for (auto& [call_key, cb] : expired)
    {
        cb(DAS_E_IPC_TIMEOUT, {}, uint16_t{0});
    }

    // SHM stale block cleanup (every 60s when idle)
    if (calls_empty)
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
    const std::unique_lock<std::mutex> lock(pending_mutex_);

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

bool IpcRunLoop::IsRunning() const { return running_.load(); }

void IpcRunLoop::SetSessionId(uint16_t session_id)
{
    local_session_id_ = session_id;
}

DasResult IpcRunLoop::PostToBusinessThread(IDasAsyncCallback* callback) noexcept
{
    if (!callback)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    if (!inbound_queue_)
    {
        DAS_CORE_LOG_ERROR(
            "PostToBusinessThread: inbound_queue_ not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    // AddRef to extend lifetime through the queue (body holds reference)
    callback->AddRef();

    // Encode callback pointer as body
    std::vector<uint8_t> body(sizeof(IDasAsyncCallback*));
    std::memcpy(body.data(), &callback, sizeof(IDasAsyncCallback*));

    // Construct header: BUSINESS_CONTROL +
    // InternalBusinessCommand::AsyncCallback
    auto header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Event)
            .SetHeaderFlags(HeaderFlags::BUSINESS_CONTROL)
            .SetInterfaceId(
                static_cast<uint32_t>(InternalBusinessCommand::AsyncCallback))
            .SetBodySize(static_cast<uint32_t>(body.size()))
            .Build();

    InboundMessage msg;
    msg.header = header;
    msg.body = std::move(body);

    DasResult push_result = inbound_queue_->Push(std::move(msg));
    if (DAS::IsFailed(push_result))
    {
        // Push failed: release the callback reference we AddRef'd above
        auto released = DasPtr<IDasAsyncCallback>::Attach(callback);
        (void)released;
        DAS_CORE_LOG_ERROR(
            "PostToBusinessThread: Push failed, result={}",
            push_result);
        return push_result;
    }

    return DAS_S_OK;
}

boost::asio::awaitable<void> IpcRunLoop::DoSendCoroutine(
    DasPtr<IHostConnection>   connection,
    ValidatedIPCMessageHeader header,
    std::vector<uint8_t>      body)
{
    try
    {
        if (!connection)
        {
            NotifySendFailure(header, DAS_E_IPC_NO_CONNECTIONS);
            co_return;
        }

        auto [lookup_result, maybe_transport] = connection->GetTransport();
        if (DAS::IsFailed(lookup_result) || !maybe_transport)
        {
            NotifySendFailure(header, lookup_result);
            co_return;
        }

        AnyTransport& transport = maybe_transport->get();
        if (!transport.IsConnected())
        {
            NotifySendFailure(header, DAS_E_IPC_CONNECTION_LOST);
            co_return;
        }

        auto result =
            co_await transport.SendCoroutine(header, body.data(), body.size());
        if (result != DAS_S_OK)
        {
            NotifySendFailure(header, result);
        }
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR("Send failed: {}", ToString(e.what()));
        NotifySendFailure(header, DAS_E_IPC_SEND_FAILED);
    }
}

boost::asio::awaitable<void> IpcRunLoop::SendToSessionCoroutine(
    ValidatedIPCMessageHeader             header,
    std::vector<uint8_t>                  body,
    PendingCallCompletion                 on_complete,
    std::chrono::steady_clock::time_point deadline)
{
    const bool has_completion = static_cast<bool>(on_complete);

    auto complete_send_failure =
        [this, &header, has_completion](DasResult error)
    {
        if (has_completion)
        {
            CompletePendingCall(
                CallKey{header.GetTargetSessionId(), header.GetCallId()},
                error,
                {},
                uint16_t{0});
        }
        else
        {
            NotifySendFailure(header, error);
        }
    };

    // Register pending call on IO thread (before send) to avoid
    // response arriving before registration.
    if (on_complete)
    {
        const CallKey call_key{header.GetTargetSessionId(), header.GetCallId()};
        const std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_[call_key] = PendingCallState{
            .call_key = call_key,
            .deadline = deadline,
            .on_complete = std::move(on_complete),
            .response_flags = 0};
    }

    const uint16_t target_session_id = header.GetTargetSessionId();
    const DasPtr<IHostConnection> host =
        connection_manager_->FindManagedHostConnection(target_session_id);

    DasResult                      lookup_result = DAS_E_IPC_OBJECT_NOT_FOUND;
    std::optional<AnyTransportRef> maybe_transport;
    if (host)
    {
        std::tie(lookup_result, maybe_transport) = host->GetTransport();
    }
    else
    {
        std::tie(lookup_result, maybe_transport) =
            connection_manager_->FindHostLocalTransport(target_session_id);
    }

    if (DAS::IsFailed(lookup_result) || !maybe_transport)
    {
        DAS_CORE_LOG_ERROR(
            "PostSend: no transport for session = {}, result = {}",
            target_session_id,
            lookup_result);
        complete_send_failure(DAS_E_IPC_NO_CONNECTIONS);
        co_return;
    }

    try
    {
        AnyTransport& transport = maybe_transport->get();
        if (!transport.IsConnected())
        {
            complete_send_failure(DAS_E_IPC_CONNECTION_LOST);
            co_return;
        }

        auto result =
            co_await transport.SendCoroutine(header, body.data(), body.size());
        if (result != DAS_S_OK)
        {
            complete_send_failure(result);
        }
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR(
            "SendToSessionCoroutine: exception: {}",
            ToString(e.what()));
        complete_send_failure(DAS_E_IPC_SEND_FAILED);
    }
}

DasResult IpcRunLoop::PostSend(
    const ValidatedIPCMessageHeader&      header,
    std::vector<uint8_t>&&                body,
    PendingCallCompletion                 on_complete,
    std::chrono::steady_clock::time_point deadline)
{
    if (!io_context_)
    {
        DAS_CORE_LOG_ERROR("PostSend: io_context_ is null");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    boost::asio::co_spawn(
        *io_context_,
        SendToSessionCoroutine(
            header,
            std::move(body),
            std::move(on_complete),
            deadline),
        boost::asio::detached);

    return DAS_S_OK;
}

DasResult IpcRunLoop::PostSendWithTransport(
    DasPtr<IHostConnection>          connection,
    const ValidatedIPCMessageHeader& header,
    std::vector<uint8_t>&&           body)
{
    if (!io_context_)
    {
        DAS_CORE_LOG_ERROR("Cannot post send because io_context is null");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    if (!connection)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    boost::asio::co_spawn(
        *io_context_,
        DoSendCoroutine(std::move(connection), header, std::move(body)),
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
        msg.header = fail_header;
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
            "failure for call_id = {}, error_code = {}",
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

    const uint32_t timeout_ms = GetNearestDeadlineMs();
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

    DasPtr<IHostConnection> host = launcher;
    auto [lookup_result, maybe_transport] = host->GetTransport();
    if (DAS::IsFailed(lookup_result) || !maybe_transport)
    {
        DAS_CORE_LOG_ERROR(
            "HostLauncher has no Transport: session_id={}, result={}",
            session_id,
            lookup_result);
        return DAS::IsFailed(lookup_result) ? lookup_result
                                            : DAS_E_IPC_NO_CONNECTIONS;
    }

    // Register with ConnectionManager
    if (!connection_manager_)
    {
        DAS_CORE_LOG_ERROR("ConnectionManager not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    auto& conn_mgr = GetConnectionManager();

    const DasResult result =
        conn_mgr.RegisterHostLauncher(session_id, std::move(launcher));
    if (result != DAS_S_OK)
    {
        return result;
    }

    StartAsyncReceiveForInternalHost(session_id, std::move(host));

    DAS_CORE_LOG_INFO("HostLauncher registered: session_id={}", session_id);
    return DAS_S_OK;
}

DasResult IpcRunLoop::RegisterInternalHost(DasPtr<IHostConnection> host)
{
    if (!host)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    if (!connection_manager_)
    {
        DAS_CORE_LOG_ERROR("ConnectionManager not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    const uint16_t session_id = host->GetSessionId();
    auto [lookup_result, maybe_transport] = host->GetTransport();
    if (DAS::IsFailed(lookup_result) || !maybe_transport)
    {
        DAS_CORE_LOG_ERROR(
            "Internal host has no transport: session_id={}, result={}",
            session_id,
            lookup_result);
        return DAS::IsFailed(lookup_result) ? lookup_result
                                            : DAS_E_IPC_NO_CONNECTIONS;
    }

    if (!maybe_transport->get().IsConnected())
    {
        DAS_CORE_LOG_ERROR(
            "Internal host transport is not connected: session_id={}",
            session_id);
        return DAS_E_IPC_NO_CONNECTIONS;
    }

    auto&           conn_mgr = GetConnectionManager();
    const DasResult result = conn_mgr.RegisterInternalHost(session_id, host);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    StartAsyncReceiveForInternalHost(session_id, std::move(host));

    DAS_CORE_LOG_INFO("Internal host registered: session_id={}", session_id);
    return DAS_S_OK;
}

void IpcRunLoop::StartAsyncReceiveForInternalHost(
    uint16_t                session_id,
    DasPtr<IHostConnection> host)
{
    if (!host)
    {
        DAS_CORE_LOG_WARN(
            "Internal host is null, skipping receive loop: session_id={}",
            session_id);
        return;
    }

    if (!io_context_)
    {
        DAS_CORE_LOG_ERROR(
            "Cannot start internal host receive loop without io_context: session_id={}",
            session_id);
        return;
    }

    boost::asio::co_spawn(
        *io_context_,
        InternalHostReceiveLoop(session_id, std::move(host)),
        boost::asio::detached);
}

boost::asio::awaitable<void> IpcRunLoop::InternalHostReceiveLoop(
    uint16_t                session_id,
    DasPtr<IHostConnection> host)
{
    // host 断连/崩溃/退出时 fail 该 session 所有 pending call，避免调用方
    // （HTTP handler / sync_wait）死等。scope guard：函数任何路径 co_return
    // 都触发析构 fail（没 pending 则返 0 无害；operation_aborted/!running 正常
    // 停止也调，但此时一般无 pending）。之前 host 崩 → receive 抛 system_error
    // → co_return，pending 原样滞留 → 前端死等，这是根因。
    struct PendingFailOnExit
    {
        IpcRunLoop* self;
        uint16_t    session_id;
        ~PendingFailOnExit()
        {
            self->FailPendingCallsBySession(
                session_id,
                DAS_E_IPC_CONNECTION_LOST);
            // 断连清扫挂载点：fail 完 pending 之后触发（回调存在时）。
            // owner 侧 holder 记账清理由 MainProcess 侧注册的回调完成，
            // 在 IO 线程执行（free-threaded DOM 表操作合法）。正常停机
            // 同样经过此路径——清扫幂等，无害。
            if (self->on_host_session_disconnected_)
            {
                self->on_host_session_disconnected_(session_id);
            }
        }
    } pending_fail_guard{this, session_id};

    while (running_.load())
    {
        if (!host)
        {
            DAS_CORE_LOG_DEBUG(
                "Internal host became null, exiting: session_id={}",
                session_id);
            co_return;
        }

        if (!connection_manager_)
        {
            DAS_CORE_LOG_ERROR(
                "Internal host receive loop has no ConnectionManager: session_id={}",
                session_id);
            co_return;
        }

        auto [lookup_result, maybe_transport] = host->GetTransport();
        if (DAS::IsFailed(lookup_result) || !maybe_transport)
        {
            DAS_CORE_LOG_ERROR(
                "Internal host transport lookup failed: session_id={}, result={}",
                session_id,
                lookup_result);
            co_return;
        }

        AnyTransport& transport = maybe_transport->get();
        if (!transport.IsConnected())
        {
            DAS_CORE_LOG_DEBUG(
                "Internal host transport disconnected, exiting: session_id={}",
                session_id);
            co_return;
        }

        try
        {
            auto result = co_await transport.ReceiveCoroutine();

            if (!running_.load())
            {
                co_return;
            }

            if (result.index() == 0)
            {
                DasResult error_code = std::get<0>(result);
                if (error_code != DAS_S_OK)
                {
                    DAS_CORE_LOG_ERROR(
                        "Internal host receive failed: session_id={}, error={}",
                        session_id,
                        error_code);
                }
                co_return;
            }

            auto&& [header, body] = std::get<1>(result);

            DAS_CORE_LOG_INFO(
                "InternalHost receive: session_id={}, msg_type={}, interface_id={}, call_id={}",
                session_id,
                static_cast<int>(header.Raw().message_type),
                header.Raw().interface_id,
                header.Raw().call_id);

            co_await RouteIncomingMessage(header, std::move(body), host);
        }
        catch (const boost::system::system_error& e)
        {
            if (e.code() == boost::asio::error::operation_aborted)
            {
                co_return;
            }
            DAS_CORE_LOG_ERROR(
                "InternalHost receive error: session_id={}, {}",
                session_id,
                e.what());
            co_return;
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_ERROR(
                "InternalHost receive exception: session_id={}, {}",
                session_id,
                e.what());
            co_return;
        }
    }
}

DAS_CORE_IPC_NS_END
