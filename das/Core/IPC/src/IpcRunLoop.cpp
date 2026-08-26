#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <das/Core/IPC/AddRefDispatchGuard.h>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/HttpIpcTransport.h>
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
#include <stdexcept>
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
    /// 连写批帧数软上限（防御性裁决：极端积压下单批段向量过大——阈值
    /// 64KB 已隐式限制单批，软上限是 polish；超限部分留下一轮循环）
    constexpr size_t OUTBOUND_COALESCE_FRAME_LIMIT = 256;

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
    RemoteObjectRegistry&            registry,
    TransportResolver                transport_resolver)
    : enable_heartbeat_(enable_heartbeat), inbound_queue_(inbound_queue),
      proxy_factory_(proxy_factory), registry_(registry),
      transport_resolver_(std::move(transport_resolver))
{
    // resolver 必填（fail-fast）：空 callable 在构造期拒绝——"没传
    // resolver 的 run loop"在构造层面无法存在，不造出来后默默走旧路
    const bool transport_resolver_is_empty = !transport_resolver_;
    if (transport_resolver_is_empty)
    {
        throw std::invalid_argument(
            "IpcRunLoop: TransportResolver must not be empty");
    }

    io_context_ = std::make_unique<boost::asio::io_context>();
    timeout_timer_ = std::make_unique<boost::asio::steady_timer>(*io_context_);

    connection_manager_ =
        std::make_unique<ConnectionManager>(1); // local_id = 1 for MainProcess

    // 心跳班车接线（零流量清账保险带）：SendHeartbeatToAll 的每 host 载体
    // 发送点入队出站 FIFO 并触发 drain（零流量场景滞留上界 = 一个心跳
    // 周期）。transport 参数保留但忽略——drain 统一经 transport_resolver_
    // 解析；空 sender 回退直发路径在 ConnectionManager 侧不动
    connection_manager_->SetHeartbeatCarrierSender(
        [this](
            AnyTransport&                    transport,
            const ValidatedIPCMessageHeader& header,
            std::vector<uint8_t>&& body) -> boost::asio::awaitable<DasResult>
        {
            (void)transport;
            const uint16_t target_session_id = header.GetTargetSessionId();
            OutboundFrame  frame;
            frame.header = header;
            frame.body = std::move(body);
            EnqueueOutboundFrame(target_session_id, std::move(frame));
            TriggerDrain(target_session_id);
            co_return DAS_S_OK;
        });

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

    // 出站 FIFO 残留帧清扫（停机面，无条件执行——~IpcRunLoop 析构
    // outbound_queues_ 前经由本函数到达）：滞留帧的完成回调从未注册进
    // pending_calls_，直接析构会静默吞掉——锁内收集、锁外以
    // CONNECTION_LOST 调用，与下方 pending_calls_ 取消块同一 fail 模式
    {
        std::vector<PendingCallCompletion> dropped_completions;
        {
            const std::unique_lock<std::mutex> lock(fifo_mutex_);
            for (auto& [session_id, queue] : outbound_queues_)
            {
                for (OutboundFrame& frame : queue.frames)
                {
                    if (frame.on_complete)
                    {
                        dropped_completions.push_back(
                            std::move(frame.on_complete));
                    }
                }
                queue.frames.clear();
            }
        }
        for (PendingCallCompletion& completion : dropped_completions)
        {
            completion(DAS_E_IPC_CONNECTION_LOST, {}, uint16_t{0});
        }
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
    // 闸3 周期末出清（IO 控制面防御漏斗）：今日该路径零物化，挂点保证
    // 将来 IO 域物化入队的帧同样在协程退出时出清（协程帧局部对象在
    // 任何 co_return 路径析构）
    const AddRefDispatchGuard add_ref_dispatch_guard(*this);

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

bool IpcRunLoop::IsForwardableFrame(
    const ValidatedIPCMessageHeader& header) const noexcept
{
    // 地址未确立的接收方一律本地消费——握手期收件人还不知道自己的
    // 地址，无权把帧判给别人（main 首条 HELLO 以本进程未来的 session
    // 定址，处理该帧时 session 才经握手落位）
    return connection_manager_ && local_session_id_ != 0
           && header.GetTargetSessionId() != 0
           && header.GetTargetSessionId() != local_session_id_;
}

boost::asio::awaitable<void> IpcRunLoop::RouteIncomingMessage(
    const ValidatedIPCMessageHeader& header,
    std::vector<uint8_t>             body,
    DasPtr<IHostConnection>          connection)
{
    // 捎带扩展段接收侧剥离（IO 协程域即时提取，分发/转发判断之前）。
    // 置位 RESERVED_AS_SIZE 时 body = [扩展段][正常 body]，扩展段 = 若干
    // 完整内嵌帧（32B header + body 连续拼接，body_size 自描述循环剥取）。
    // 转发兼容：转发是不透明字节搬运——在途帧（收件地址非本地，含组合
    // 控制面帧）整帧原样透传、由终点递归剥离；剥离判定与转发分支共用
    // 同一地址谓词 IsForwardableFrame（单一判定，无第二份分歧判断）。
    if ((header.GetFlags() & MessageFlags::RESERVED_AS_SIZE) != 0)
    {
        const bool is_transit_frame = IsForwardableFrame(header);

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

    // 控制面帧仅当收件地址为本地（或未定址 0）才进本地消费——按地址
    // 分类，不按消息类别（握手/心跳的收件人恒为直连对端，天然本地；
    // 引用申报帧发给 owner 即转 owner）
    if (IsControlPlaneMessage(header) && !IsForwardableFrame(header))
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

    if (IsForwardableFrame(header))
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

void IpcRunLoop::SetPiggybackEnabled(bool enabled)
{
    piggyback_enabled_ = enabled;
}

bool IpcRunLoop::IsPiggybackEnabled() const noexcept
{
    return piggyback_enabled_;
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
        // 停机窗口（队列未初始化/已关闭）：为入队 AddRef 的引用有意
        // 不在此释放——本函数调用域在 IO 线程，原地析构回调会把它
        // 捕获的对象 DasPtr 一并 final Release，违反“真实对象析构
        // 不落在 IO 线程”不变量。有界泄漏，由进程退出统一回收。
        DAS_CORE_LOG_ERROR(
            "PostToBusinessThread: Push failed, callback intentionally "
            "leaked until process exit, result = {}",
            push_result);
        return push_result;
    }

    return DAS_S_OK;
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

    // 业务帧入队即触发 drain（只带走已入队帧——保证无后续事件时响应
    // 不被压到下一周期）。完成回调与 deadline 随帧携带，由 drain 在
    // 物理写前注册 pending_calls_
    OutboundFrame frame;
    frame.header = header;
    frame.body = std::move(body);
    frame.on_complete = std::move(on_complete);
    frame.deadline = deadline;
    const uint16_t target_session_id = frame.header.GetTargetSessionId();
    EnqueueOutboundFrame(target_session_id, std::move(frame));
    TriggerDrain(target_session_id);

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

    // connection 参数弱化为路由提示（resolver 统一解析裁决）：drain 经
    // transport_resolver_(target) 两级查表——第一条即 FindManagedHost
    // Connection，与显式持 connection 的路由结果一致；FIFO 化后所有
    // 出站帧共用同一 drain 写路径
    OutboundFrame frame;
    frame.header = header;
    frame.body = std::move(body);
    const uint16_t target_session_id = frame.header.GetTargetSessionId();
    EnqueueOutboundFrame(target_session_id, std::move(frame));
    TriggerDrain(target_session_id);

    return DAS_S_OK;
}

OutboundFrame IpcRunLoop::BuildAddRefFrame(const ObjectId& object_id)
{
    std::vector<uint8_t> add_ref_body;
    add_ref_body.insert(
        add_ref_body.end(),
        reinterpret_cast<const uint8_t*>(&object_id),
        reinterpret_cast<const uint8_t*>(&object_id) + sizeof(object_id));

    auto add_ref_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Event)
            .SetHeaderFlags(HeaderFlags::CONTROL_PLANE)
            .SetInterfaceId(static_cast<uint32_t>(IpcCommandType::RemoteAddRef))
            .SetSourceSessionId(local_session_id_)
            .SetTargetSessionId(object_id.session_id)
            .SetBodySize(static_cast<uint32_t>(add_ref_body.size()))
            .Build();

    return {std::move(add_ref_header), std::move(add_ref_body)};
}

void IpcRunLoop::EnqueueAddRef(const ObjectId& object_id)
{
    // BT 物化语义：入队不触发（同周期 Response 带走——保证每调用一次
    // 物理写；触发即独写 [ADD_REF] 会使随后的 Response 变第二写）。
    // 叶子契约原样继承：构帧在锁外、入队自带 mutex 零回调零 IO——
    // proxy_cache_mutex_ 持锁区内调用安全（锁序单向 proxy_cache_mutex_
    // → fifo_mutex_）
    OutboundFrame frame = BuildAddRefFrame(object_id);
    EnqueueOutboundFrame(object_id.session_id, std::move(frame));
}

DasResult IpcRunLoop::PostAddRefStandalone(const ObjectId& object_id)
{
    // 非 BT 物化：入队 + 立即触发——延迟等价旧直发，单一 drain 写者
    // 取代旁路直发（禁止双写者竞态帧序）。发送失败不回滚注册
    // （fire-and-forget，owner 侧断连清扫兜底）
    if (!io_context_)
    {
        return DAS_E_IPC_NOT_INITIALIZED;
    }
    OutboundFrame  frame = BuildAddRefFrame(object_id);
    const uint16_t target_session_id = frame.header.GetTargetSessionId();
    EnqueueOutboundFrame(target_session_id, std::move(frame));
    TriggerDrain(target_session_id);
    return DAS_S_OK;
}

OutboundFrame IpcRunLoop::BuildReleaseFrame(const ObjectId& object_id)
{
    std::vector<uint8_t> release_body;
    release_body.insert(
        release_body.end(),
        reinterpret_cast<const uint8_t*>(&object_id),
        reinterpret_cast<const uint8_t*>(&object_id) + sizeof(object_id));

    auto release_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Event)
            .SetHeaderFlags(HeaderFlags::CONTROL_PLANE)
            .SetInterfaceId(
                static_cast<uint32_t>(IpcCommandType::RemoteRelease))
            .SetSourceSessionId(local_session_id_)
            .SetTargetSessionId(object_id.session_id)
            .SetBodySize(static_cast<uint32_t>(release_body.size()))
            .Build();

    return {std::move(release_header), std::move(release_body)};
}

void IpcRunLoop::EnqueueReleaseFrame(const ObjectId& object_id)
{
    // DasProxyBase 析构的唯一 RELEASE 出口：入队参与 FIFO（fire-and-forget
    // 语义不变，owner 侧条目拆除延迟上界 = 周期末）。触发矩阵：BT 线程
    // 不触发（[ADD_REF][RELEASE] 等同周期 Response 带走）；非 BT 线程
    // 触发（无 guard 兜底，滞留上界不能退化为心跳周期）。错判只是
    // 性能/延迟问题非正确性问题
    OutboundFrame  frame = BuildReleaseFrame(object_id);
    const uint16_t target_session_id = frame.header.GetTargetSessionId();
    EnqueueOutboundFrame(target_session_id, std::move(frame));
    if (!IsCurrentBusinessThread())
    {
        TriggerDrain(target_session_id);
    }
}

void IpcRunLoop::EnqueueOutboundFrame(
    uint16_t        session_id,
    OutboundFrame&& frame)
{
    // 唯一入队叶子（P-9 契约）：自带 mutex、锁内仅 move 入队，零回调
    // 零 IO 零 co_spawn——proxy_cache_mutex_ 持锁区内调用安全（锁序单向
    // proxy_cache_mutex_ → fifo_mutex_）
    const std::unique_lock<std::mutex> lock(fifo_mutex_);
    outbound_queues_[session_id].frames.push_back(std::move(frame));
}

void IpcRunLoop::TriggerDrain(uint16_t session_id)
{
    {
        const std::unique_lock<std::mutex> lock(fifo_mutex_);
        OutboundSessionQueue&              queue = outbound_queues_[session_id];
        if (queue.drain_active)
        {
            queue.drain_retrigger = true;
            return;
        }
        queue.drain_active = true;
    }

    if (!io_context_)
    {
        // 停机窗口（io_context_ 缺失/已析构）：复位单飞标志，滞留帧由
        // 进程退出回收（fire-and-forget 语义）
        const std::unique_lock<std::mutex> lock(fifo_mutex_);
        const auto session_entry = outbound_queues_.find(session_id);
        if (session_entry != outbound_queues_.end())
        {
            session_entry->second.drain_active = false;
        }
        return;
    }

    // co_spawn 在 fifo_mutex_ 临界区之外（锁内零分配零 post）
    boost::asio::co_spawn(
        *io_context_,
        DrainOutboundCoroutine(session_id),
        boost::asio::detached);
}

boost::asio::awaitable<void> IpcRunLoop::DrainOutboundCoroutine(
    uint16_t session_id)
{
    for (;;)
    {
        std::vector<OutboundFrame> frames;
        {
            const std::unique_lock<std::mutex> lock(fifo_mutex_);
            const auto session_entry = outbound_queues_.find(session_id);
            if (session_entry == outbound_queues_.end())
            {
                // 已被断连清扫（ClearOutboundFifo erase）：直接收尾
                co_return;
            }
            OutboundSessionQueue& queue = session_entry->second;
            if (queue.frames.empty())
            {
                if (queue.drain_retrigger)
                {
                    queue.drain_retrigger = false;
                    continue; // 竞速窗口内又有帧入队：不丢醒
                }
                outbound_queues_.erase(session_entry); // 空且无重触发：回收
                co_return;
            }
            // 取帧与清 retrigger 同锁原子：本轮整体取走，之后入队的帧
            // 由 retrigger 语义交由下一轮
            queue.drain_retrigger = false;
            frames.reserve(queue.frames.size());
            for (OutboundFrame& frame : queue.frames)
            {
                frames.push_back(std::move(frame));
            }
            queue.frames.clear();
        }

        // 带完成回调的帧先注册 pending_calls_（IO 线程、物理写前——消除
        // 响应早于注册的竞态）
        for (OutboundFrame& frame : frames)
        {
            if (frame.on_complete)
            {
                const CallKey call_key{
                    frame.header.GetTargetSessionId(),
                    frame.header.GetCallId()};
                const std::unique_lock<std::mutex> lock(pending_mutex_);
                pending_calls_[call_key] = PendingCallState{
                    .call_key = call_key,
                    .deadline = frame.deadline,
                    .on_complete = std::move(frame.on_complete),
                    .response_flags = 0};
                frame.pending_call_registered = true;
            }
        }

        try
        {
            co_await WriteFramesCoalesced(session_id, std::move(frames));
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_ERROR(
                "DrainOutboundCoroutine: exception: {}",
                ToString(e.what()));
        }
    }
}

bool IpcRunLoop::IsStandaloneOutboundFrame(
    const OutboundFrame& frame) const noexcept
{
    // 转发帧（不透明搬运裁决：独走，不进连写批也不作拼装宿主——
    // 保守且与“不透明搬运”语义的表意一致）
    if (frame.header.GetSourceSessionId() != local_session_id_)
    {
        return true;
    }
    // 经本机转发的拼装帧（对端 bit1 帧）：深度=1 接收侧硬约束禁止再
    // 拼装（P-3）
    if ((frame.header.GetFlags() & MessageFlags::RESERVED_AS_SIZE) != 0)
    {
        return true;
    }
    // SHM 帧（转发的 LARGE_MESSAGE_VIA_SHM）：扩展段/连写批不经传输层
    // SHM 重组，handle 字节不可当真实 body 混入（P-2）
    if ((frame.header.GetHeaderFlags() & HeaderFlags::LARGE_MESSAGE_VIA_SHM)
        != 0)
    {
        return true;
    }
    // 本地大帧：按 body_size 判（SHM 位是 SendCoroutine 内部才置的，
    // 入队时未置——P-2）；独走让传输层做 SHM 变换
    if (sizeof(IPCMessageHeader) + frame.body.size() >= LARGE_MESSAGE_THRESHOLD)
    {
        return true;
    }
    return false;
}

boost::asio::awaitable<void> IpcRunLoop::WriteFramesCoalesced(
    uint16_t                     session_id,
    std::vector<OutboundFrame>&& frames)
{
    // frames 容器 move 进本协程帧（SendSegment 非拥有契约，P-1：批内
    // 禁止再按引用切子批——段备存储存活至 async_write 完成）
    std::vector<OutboundFrame> owned_frames = std::move(frames);

    auto fail_frame = [this](OutboundFrame& frame, DasResult error)
    {
        if (frame.pending_call_registered)
        {
            CompletePendingCall(
                CallKey{
                    frame.header.GetTargetSessionId(),
                    frame.header.GetCallId()},
                error,
                {},
                uint16_t{0});
            return;
        }
        if (IsControlPlaneMessage(frame.header))
        {
            // 控制面 fire-and-forget 帧：仅告警（owner 侧断连清扫兜底）
            DAS_CORE_LOG_WARN(
                "Outbound control frame dropped: interface_id = 0x{:08X}, "
                "result = {}",
                frame.header.GetInterfaceId(),
                error);
            return;
        }
        NotifySendFailure(frame.header, error);
    };

    // 解析单点（统一走 resolver 裁决）：drain 持非拥有 AnyTransportRef，
    // 寿命由 ConnectionManager 表条目 + 接收协程持有的 host DasPtr 覆盖
    const auto [lookup_result, maybe_transport] =
        transport_resolver_(session_id);
    if (DAS::IsFailed(lookup_result) || !maybe_transport)
    {
        DAS_CORE_LOG_ERROR(
            "Drain: no transport for session = {}, result = {}",
            session_id,
            lookup_result);
        for (OutboundFrame& frame : owned_frames)
        {
            fail_frame(frame, DAS_E_IPC_NO_CONNECTIONS);
        }
        co_return;
    }

    try
    {
        AnyTransport& transport = maybe_transport->get();
        if (!transport.IsConnected())
        {
            for (OutboundFrame& frame : owned_frames)
            {
                fail_frame(frame, DAS_E_IPC_CONNECTION_LOST);
            }
            co_return;
        }

        // Http 扩展段拼装的骑手预算：每骑手整帧两段（header+body）+
        // 宿主 body 一段，段总数不超过 Http SendScatter 的段数上限
        const size_t max_carried_riders =
            (HttpIpcTransport::MAX_SCATTER_SEGMENTS - 1) / 2;

        size_t batch_begin = 0;
        while (batch_begin < owned_frames.size())
        {
            // 切断扫描：[batch_begin, batch_end) 为一个合写批。特殊帧
            // 独立成批（独走 SendCoroutine）；piggyback 关闭时不累积
            // （全部逐帧出——off 必须仍走 FIFO 只是逐帧发，禁止旁路
            // 直发重新引入双写者）
            size_t batch_end = batch_begin + 1;
            if (!IsStandaloneOutboundFrame(owned_frames[batch_begin])
                && piggyback_enabled_)
            {
                size_t batch_total_size =
                    sizeof(IPCMessageHeader)
                    + owned_frames[batch_begin].body.size();
                while (batch_end < owned_frames.size())
                {
                    const OutboundFrame& candidate = owned_frames[batch_end];
                    if (IsStandaloneOutboundFrame(candidate))
                    {
                        break;
                    }
                    const size_t candidate_frame_size =
                        sizeof(IPCMessageHeader) + candidate.body.size();
                    if (batch_end - batch_begin
                        >= OUTBOUND_COALESCE_FRAME_LIMIT)
                    {
                        break;
                    }
                    // Http 骑手预算止批：溢出部分由外层 while 同轮下一批
                    // 继续拼装（批批有序，不跨轮滞留）
                    if (!transport.CanBatchFrames()
                        && batch_end - batch_begin >= max_carried_riders + 1)
                    {
                        break;
                    }
                    if (batch_total_size + candidate_frame_size
                        >= LARGE_MESSAGE_THRESHOLD)
                    {
                        break;
                    }
                    batch_total_size += candidate_frame_size;
                    ++batch_end;
                }
            }

            const size_t batch_frame_count = batch_end - batch_begin;
            const bool   can_batch =
                batch_frame_count > 1 && transport.CanBatchFrames();

            if (can_batch)
            {
                // D-20 连写：首帧拆 header 参数 + body 段、其余帧整帧
                // 两段（SendScatter header 恒在线首的调用形态约定），
                // 一次 async_write；段序 = FIFO
                std::vector<SendSegment> segments;
                segments.reserve(2 * batch_frame_count);
                const OutboundFrame& first_frame = owned_frames[batch_begin];
                segments.push_back(
                    SendSegment{
                        first_frame.body.data(),
                        first_frame.body.size()});
                for (size_t index = batch_begin + 1; index < batch_end; ++index)
                {
                    const OutboundFrame& frame = owned_frames[index];
                    segments.push_back(
                        SendSegment{
                            reinterpret_cast<const uint8_t*>(
                                static_cast<const IPCMessageHeader*>(
                                    frame.header)),
                            sizeof(IPCMessageHeader)});
                    segments.push_back(
                        SendSegment{frame.body.data(), frame.body.size()});
                }

                const DasResult send_result = co_await transport.SendScatter(
                    first_frame.header,
                    std::move(segments));
                if (send_result == DAS_S_OK)
                {
                    outbound_physical_writes_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    outbound_logical_frames_.fetch_add(
                        batch_frame_count,
                        std::memory_order_relaxed);
                }
                else
                {
                    for (size_t index = batch_begin; index < batch_end; ++index)
                    {
                        fail_frame(owned_frames[index], send_result);
                    }
                }
            }
            else if (batch_frame_count > 1)
            {
                // Http 扩展段拼装（一条 WS 消息 = 一个 IPC 帧）：末帧为
                // 宿主、其余帧按序内嵌扩展段——外层 header 改写置
                // RESERVED_AS_SIZE、reserved=段总字节、body_size 增段总长
                // （Response 帧保留 error_code 字段语义），段序列 = 骑手
                // 整帧在前（每帧 header+body 两段）+ 宿主 body 段收尾。
                // 接收侧 RouteIncomingMessage 剥离时内嵌帧先于宿主递归
                // 派发，与入队序严格对应；Http SendScatter 入口断言外层
                // header 已置位（本分支即其唯一合法喂入方）
                const OutboundFrame& host_frame = owned_frames[batch_end - 1];
                size_t               section_size = 0;
                for (size_t index = batch_begin; index + 1 < batch_end; ++index)
                {
                    const OutboundFrame& rider = owned_frames[index];
                    section_size +=
                        sizeof(IPCMessageHeader) + rider.body.size();
                }

                const IPCMessageHeader  raw = host_frame.header.Raw();
                IPCMessageHeaderBuilder outer_builder;
                outer_builder
                    .SetMessageType(static_cast<MessageType>(raw.message_type))
                    .SetHeaderFlags(raw.header_flags)
                    .SetInterfaceId(raw.interface_id)
                    .SetCallId(raw.call_id)
                    .SetSourceSessionId(raw.source_session_id)
                    .SetTargetSessionId(raw.target_session_id)
                    .SetFlags(
                        static_cast<uint16_t>(
                            raw.flags | MessageFlags::RESERVED_AS_SIZE))
                    .SetBodySize(
                        static_cast<uint32_t>(raw.body_size + section_size))
                    .SetReserved(static_cast<uint32_t>(section_size));
                if (raw.message_type
                    == static_cast<uint8_t>(MessageType::Response))
                {
                    // 错误响应的 error_code 不能在改写中丢失
                    outer_builder.SetErrorCode(raw.error_code);
                }
                const ValidatedIPCMessageHeader outer_header =
                    outer_builder.Build();

                std::vector<SendSegment> segments;
                segments.reserve(2 * (batch_frame_count - 1) + 1);
                for (size_t index = batch_begin; index + 1 < batch_end; ++index)
                {
                    const OutboundFrame& rider = owned_frames[index];
                    segments.push_back(
                        SendSegment{
                            reinterpret_cast<const uint8_t*>(
                                static_cast<const IPCMessageHeader*>(
                                    rider.header)),
                            sizeof(IPCMessageHeader)});
                    segments.push_back(
                        SendSegment{rider.body.data(), rider.body.size()});
                }
                segments.push_back(
                    SendSegment{
                        host_frame.body.data(),
                        host_frame.body.size()});

                const DasResult send_result = co_await transport.SendScatter(
                    outer_header,
                    std::move(segments));
                if (send_result == DAS_S_OK)
                {
                    outbound_physical_writes_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    outbound_logical_frames_.fetch_add(
                        batch_frame_count,
                        std::memory_order_relaxed);
                }
                else
                {
                    for (size_t index = batch_begin; index < batch_end; ++index)
                    {
                        fail_frame(owned_frames[index], send_result);
                    }
                }
            }
            else
            {
                // 逐帧 SendCoroutine：单帧批、piggyback off 与特殊帧独走
                // 的共同路径
                for (size_t index = batch_begin; index < batch_end; ++index)
                {
                    OutboundFrame&  frame = owned_frames[index];
                    const DasResult send_result =
                        co_await transport.SendCoroutine(
                            frame.header,
                            frame.body.data(),
                            frame.body.size());
                    if (send_result == DAS_S_OK)
                    {
                        outbound_physical_writes_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                        outbound_logical_frames_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }
                    else
                    {
                        fail_frame(frame, send_result);
                    }
                }
            }
            batch_begin = batch_end;
        }
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR(
            "WriteFramesCoalesced: exception: {}",
            ToString(e.what()));
        for (OutboundFrame& frame : owned_frames)
        {
            fail_frame(frame, DAS_E_IPC_SEND_FAILED);
        }
    }
}

size_t IpcRunLoop::FlushOutboundFifo()
{
    // guard 周期末兜底：对全部有滞留帧的 session 触发一次 drain（合写
    // 形态出清）——控制帧滞留上界 = 一个 dispatch 周期
    std::vector<uint16_t> sessions_to_drain;
    size_t                queued_frame_count = 0;
    {
        const std::unique_lock<std::mutex> lock(fifo_mutex_);
        for (const auto& [session_id, queue] : outbound_queues_)
        {
            if (!queue.frames.empty())
            {
                sessions_to_drain.push_back(session_id);
                queued_frame_count += queue.frames.size();
            }
        }
    }
    for (const uint16_t session_id : sessions_to_drain)
    {
        TriggerDrain(session_id);
    }
    return queued_frame_count;
}

size_t IpcRunLoop::ClearOutboundFifo(uint16_t session_id)
{
    // 滞留帧携带的完成回调尚未注册进 pending_calls_（注册推迟到 drain
    // 取帧之后），静默销毁会把调用方压到 wait() 的 30s 外部兜底并误报
    // DAS_E_IPC_TIMEOUT——锁内先收集、锁外以 CONNECTION_LOST 直接调用
    // （未注册条目不能走 CompletePendingCall），与 FailPendingCallsBySession
    // 的断连快速失败契约对齐
    std::vector<PendingCallCompletion> dropped_completions;
    size_t                             cleared_count = 0;
    {
        const std::unique_lock<std::mutex> lock(fifo_mutex_);
        const auto session_entry = outbound_queues_.find(session_id);
        if (session_entry == outbound_queues_.end())
        {
            return 0;
        }
        cleared_count = session_entry->second.frames.size();
        for (OutboundFrame& frame : session_entry->second.frames)
        {
            if (frame.on_complete)
            {
                dropped_completions.push_back(std::move(frame.on_complete));
            }
        }
        if (session_entry->second.drain_active)
        {
            // 活跃 drain 收尾自回收（成对实现）：只清 frames 不 erase 条目
            // ——drain 循环按空队/retrigger 语义自行退出并 erase
            session_entry->second.frames.clear();
        }
        else
        {
            outbound_queues_.erase(session_entry);
        }
    }
    for (PendingCallCompletion& completion : dropped_completions)
    {
        completion(DAS_E_IPC_CONNECTION_LOST, {}, uint16_t{0});
    }
    return cleared_count;
}

size_t IpcRunLoop::GetPendingAddRefCount(uint16_t session_id) const
{
    // 语义重定义：出站 FIFO 深度（已入队未写出的帧数）——测试观测面
    // 同名保留（38+ 处引用零改动）
    const std::unique_lock<std::mutex> lock(fifo_mutex_);
    const auto session_entry = outbound_queues_.find(session_id);
    if (session_entry == outbound_queues_.end())
    {
        return 0;
    }
    return session_entry->second.frames.size();
}

bool IpcRunLoop::PendingAddRefsEmpty() const
{
    // 语义重定义：全部 session 的出站 FIFO 均无滞留帧
    const std::unique_lock<std::mutex> lock(fifo_mutex_);
    for (const auto& [session_id, queue] : outbound_queues_)
    {
        (void)session_id;
        if (!queue.frames.empty())
        {
            return false;
        }
    }
    return true;
}

void IpcRunLoop::NotifySendFailure(
    const ValidatedIPCMessageHeader& header,
    DasResult                        error_code)
{
    // transit 分支：source 非本地的帧是经本进程中转的在途帧——失败通知
    // 的收件人是原发起方（source），不是本地。Request 照下方本地路径同一
    // builder 模板合成失败 Response 改投原 source；Event/Response 无
    // CallKey 配对语义，丢弃 + WARN（不限流——每条 WARN 对应一次真实的
    // 死目标流量，信号不藏）。错误码统一 CONNECTION_LOST：星型拓扑下
    // 查表 miss 与传输写失败同为“目标 session 已死/不可达”（含触发
    // 原因为 NO_CONNECTIONS 的场景）。递归安全：合成帧自身是 Response，
    // 回投腿也失败时再入本分支走 Event/Response 丢弃路径终止。
    // 路由一致性：合成帧经 PostSend 入队 FIFO、由 drain 统一写出，转发
    // 形态（source = 原 target ≠ 本地）被切断扫描判为独走帧、原样透传，
    // 无需额外接线。
    if (header.GetSourceSessionId() != local_session_id_)
    {
        if (header.GetMessageType() == MessageType::Request)
        {
            // RESPONSE 的 source/target 必须与 REQUEST 互换（与 IStubBase
            // 一致），否则 CallKey 匹配不到发起方 pending_calls_ 中的条目；
            // body 为空——错误码仅在 header
            auto fail_header =
                IPCMessageHeaderBuilder()
                    .SetCallId(header.GetCallId())
                    .SetSourceSessionId(header.GetTargetSessionId())
                    .SetTargetSessionId(header.GetSourceSessionId())
                    .SetErrorCode(
                        static_cast<int32_t>(DAS_E_IPC_CONNECTION_LOST))
                    .Build();
            PostSend(fail_header, std::vector<uint8_t>{});
            return;
        }

        DAS_CORE_LOG_WARN(
            "Dropped unreachable transit frame: message_type = {}, "
            "target = {}, source = {}, interface_id = 0x{:08X}, call_id = {}",
            static_cast<int>(header.GetMessageType()),
            header.GetTargetSessionId(),
            header.GetSourceSessionId(),
            header.GetInterfaceId(),
            header.GetCallId());
        return;
    }

    // 本地起源路径：构造失败 RESPONSE，推入 inbound_queue_
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
            // 断连清队（主侧触发链）：该 session 的出站 FIFO 帧不再
            // 可达，随断连清空防滞留（fire-and-forget 语义下迟到帧本就
            // 无害——此为确定性清理，不等超时）；活跃 drain 由其收尾
            // 自回收条目
            static_cast<void>(self->ClearOutboundFifo(session_id));
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
