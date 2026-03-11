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
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>

#ifdef _WIN32
#include <das/Core/IPC/Win32AsyncIpcTransport.h>
#else
#include <das/Core/IPC/UnixAsyncIpcTransport.h>
#endif

#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

// 工厂函数实现
DAS::Utils::Expected<std::unique_ptr<IpcRunLoop>> IpcRunLoop::Create()
{
    auto instance = std::unique_ptr<IpcRunLoop>(new IpcRunLoop());
    auto result = instance->DoInitialize();
    if (result != DAS_S_OK)
    {
        return DAS::Utils::MakeUnexpected(result);
    }
    return instance;
}

DAS::Utils::Expected<std::unique_ptr<IpcRunLoop>> IpcRunLoop::CreateForHost(
    const std::string& read_queue_name,
    const std::string& write_queue_name,
    bool               is_server)
{
    // 已废弃：Host 模式应由 IpcContext 持有 transport
    DAS_CORE_LOG_WARN(
        "CreateForHost is deprecated. Use Create() and manage transport externally.");
    (void)read_queue_name;
    (void)write_queue_name;
    (void)is_server;

    // 返回错误，提示调用者使用新的模式
    return DAS::Utils::MakeUnexpected(DAS_E_NOT_FOUND);
}

IpcRunLoop::~IpcRunLoop()
{
    // 正确调用 Uninitialize() 而非 RequestStop()
    Uninitialize();
}

DasResult IpcRunLoop::DoInitialize()
{
    io_context_ = std::make_unique<boost::asio::io_context>();
    // 不再创建 async_transport_，IpcRunLoop 只提供 io_context 基础设施
    // 所有 transport 由外部管理：
    // - MainProcess 模式：HostLauncher 持有 transport
    // - Host 模式：IpcContext 持有 transport
    timeout_timer_ = std::make_unique<boost::asio::steady_timer>(*io_context_);

    // Create ConnectionManager for MainProcess mode
    // In Host mode, connection_manager_ remains nullptr (no heartbeat needed)
    connection_manager_ = ConnectionManager::Create(1);  // local_id = 1 for MainProcess

    return DAS_S_OK;
}

void IpcRunLoop::Uninitialize()
{
    RequestStop();

    // 清理 ConnectionManager
    // unique_ptr will automatically call destructor which calls Uninitialize()
    if (connection_manager_)
    {
        connection_manager_->StopHeartbeatThread();
        connection_manager_.reset();
    }

    // 确保所有异步操作都已完成
    // 如果 io_context 仍在运行（理论上不应该）。强制停止
    if (io_context_ && !io_context_->stopped())
    {
        io_context_->stop();
    }

    work_guard_.reset();
    timeout_timer_.reset();
    // async_transport_ 已移除，不再重置
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
            for (auto& [call_id, state] : pending_calls_)
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
            cb(DAS_E_IPC_CONNECTION_LOST, {});
        }
    }

    // 注意：RequestStop() 是非阻塞的，不调用 io_context_->stop()
    // 让协程自然退出后，io_context_->run() 会因为没有更多工作而返回
}

void IpcRunLoop::RegisterHandler(std::unique_ptr<IMessageHandler> handler)
{
    if (!handler)
    {
        return;
    }
    handlers_[handler->GetInterfaceId()] = std::move(handler);
}

void IpcRunLoop::RegisterControlPlaneHandler(std::unique_ptr<IMessageHandler> handler)
{
    if (!handler)
    {
        return;
    }
    control_plane_handler_ = std::move(handler);
}

IMessageHandler* IpcRunLoop::GetHandler(uint32_t interface_id) const
{
    auto it = handlers_.find(interface_id);
    if (it != handlers_.end())
    {
        return it->second.get();
    }
    return nullptr;
}

boost::asio::awaitable<void> IpcRunLoop::DispatchToHandlerCoroutine(
    const IPCMessageHeader&     header,
    const std::vector<uint8_t>& body,
#ifdef _WIN32
    Win32AsyncIpcTransport&     transport)
#else
    UnixAsyncIpcTransport&      transport)
#endif
{
    DAS_CORE_LOG_INFO(
        "DispatchToHandlerCoroutine: interface_id={}, method_id={}, call_id={}, body_size={}",
        header.interface_id,
        header.method_id,
        header.call_id,
        body.size());

    // 处理 HEARTBEAT RESPONSE：收到心跳回复时更新时间戳
    if (header.interface_id
        == static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_HEARTBEAT))
    {
        if (header.message_type == static_cast<uint8_t>(MessageType::RESPONSE))
        {
            // 收到心跳回复，更新时间戳
            if (connection_manager_)
            {
                connection_manager_->UpdateHeartbeatTimestamp(header.session_id);
            }
            co_return;
        }
        // REQUEST 继续交给 handler 处理
    }

    IMessageHandler* handler = nullptr;

    // 首先检查是否是控制平面消息（interface_id 1-7）
    if (IpcHeaderValidator::IsControlPlane(header))
    {
        // 控制平面消息使用专门的控制平面 handler
        handler = control_plane_handler_.get();
        if (handler)
        {
            DAS_CORE_LOG_INFO(
                "DispatchToHandlerCoroutine: using control plane handler for interface_id={}",
                header.interface_id);
        }
    }

    // 如果不是控制平面消息，或者没有控制平面 handler，则使用普通 handler
    if (!handler)
    {
        handler = GetHandler(header.interface_id);
    }

    if (handler)
    {
        DAS_CORE_LOG_INFO("DispatchToHandlerCoroutine: found handler for interface_id={}", header.interface_id);
        IpcResponseSender sender(transport);
        auto result = co_await handler->HandleMessage(header, body, sender);
        if (DAS::IsFailed(result))
        {
            DAS_CORE_LOG_WARN("Handler returned error: 0x{:08X}", result);
        }
        else
        {
            DAS_CORE_LOG_INFO("DispatchToHandlerCoroutine: handler completed successfully");
        }
    }
    else
    {
        DAS_CORE_LOG_WARN(
            "No handler found for interface_id = {}",
            header.interface_id);
    }
}

std::pair<DasResult, uint64_t> IpcRunLoop::PrepareSendRequestWithTransport(
    DefaultAsyncIpcTransport*        transport,
    const ValidatedIPCMessageHeader& request_header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    if (!transport)
    {
        return {DAS_E_INVALID_ARGUMENT, 0};
    }

    uint64_t call_id = next_call_id_.fetch_add(1);

    const IPCMessageHeader& raw = request_header.Raw();
    auto                    validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::REQUEST)
            .SetBusinessInterface(raw.interface_id, raw.method_id)
            .SetBodySize(raw.body_size)
            .SetCallId(call_id)
            .SetObject(raw.session_id, raw.generation, raw.local_id)
            .Build();

    {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_[call_id] = PendingCallState{
            call_id,
            {},
            std::chrono::steady_clock::time_point::max(),
            nullptr};
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
            pending_calls_.erase(call_id);
            return {send_result, 0};
        }

        return {send_result, call_id};
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR("Send failed: {}", e.what());
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_.erase(call_id);
        return {DAS_E_IPC_CONNECTION_LOST, 0};
    }
}

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
            return;
        }

        on_complete = std::move(it->second.on_complete);
        pending_calls_.erase(it);
    }

    if (on_complete)
    {
        on_complete(result, std::move(response));
    }
}

void IpcRunLoop::TickPendingSenders()
{
    auto now = std::chrono::steady_clock::now();

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
        return 0;
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
    uint64_t                              call_id,
    std::chrono::steady_clock::time_point deadline,
    PendingCallCompletion                 on_complete)
{
    {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        auto                         it = pending_calls_.find(call_id);
        if (it != pending_calls_.end())
        {
            it->second.deadline = deadline;
            it->second.on_complete = std::move(on_complete);
            return;
        }
    }

    if (on_complete)
    {
        on_complete(DAS_E_IPC_TIMEOUT, {});
    }
}

bool IpcRunLoop::IsRunning() const { return running_.load(); }

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
    work_guard_ = std::make_unique<WorkGuard>(io_context_->get_executor());

    // 启动心跳检测线程（MainProcess 模式）
    if (connection_manager_)
    {
        connection_manager_->StartHeartbeatThread();
    }

    // 启动超时检查定时器
    ScheduleTimeoutCheck();

    // 阻塞运行事件循环
    io_context_->run();

    return exit_code_.load();
}

DasResult IpcRunLoop::RegisterHostLauncher(DasPtr<IHostLauncher> launcher)
{
    if (!launcher)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    uint16_t session_id = launcher->GetSessionId();

    // 获取具体 HostLauncher 类型以访问 GetTransport
    auto* concrete = dynamic_cast<HostLauncher*>(launcher.Get());
    if (!concrete)
    {
        DAS_CORE_LOG_ERROR("Failed to cast IHostLauncher to HostLauncher: session_id={}", session_id);
        return DAS_E_INVALID_ARGUMENT;
    }

    auto* transport = concrete->GetTransport();
    if (!transport)
    {
        DAS_CORE_LOG_ERROR("HostLauncher has no Transport: session_id={}", session_id);
        return DAS_E_IPC_NO_CONNECTIONS;
    }

    // Register with ConnectionManager
    auto* conn_mgr = GetConnectionManager();
    if (!conn_mgr)
    {
        DAS_CORE_LOG_ERROR("ConnectionManager not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    DasResult result = conn_mgr->RegisterHostLauncher(session_id, std::move(launcher));
    if (result != DAS_S_OK)
    {
        return result;
    }

    // Start async receive for this Transport
    // 注意：从 ConnectionManager 获取 DasPtr 副本，协程会捕获它来保持 transport 存活
    StartAsyncReceiveForTransport(session_id, conn_mgr->GetLauncher(session_id));

    DAS_CORE_LOG_INFO("HostLauncher registered: session_id={}", session_id);
    return DAS_S_OK;
}

void IpcRunLoop::StartAsyncReceiveForTransport(
    uint16_t                     session_id,
    DasPtr<IHostLauncher> launcher)
{
    if (!launcher)
    {
        DAS_CORE_LOG_WARN(
            "Launcher is null, skipping receive loop: session_id={}",
            session_id);
        return;
    }

    // 获取具体 HostLauncher 类型以访问 GetTransport
    auto* concrete = dynamic_cast<HostLauncher*>(launcher.Get());
    if (!concrete)
    {
        DAS_CORE_LOG_WARN(
            "Failed to cast IHostLauncher to HostLauncher: session_id={}",
            session_id);
        return;
    }

    auto* transport = concrete->GetTransport();
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
                    // 从 launcher 获取 transport（安全，因为 launcher 被 DasPtr 持有）
                    auto* concrete = dynamic_cast<HostLauncher*>(launcher.Get());
                    if (!concrete)
                    {
                        DAS_CORE_LOG_DEBUG("Failed to cast launcher, exiting: session_id={}", session_id);
                        co_return;
                    }

                    auto* transport = concrete->GetTransport();
                    if (!transport)
                    {
                        DAS_CORE_LOG_DEBUG("Transport became null, exiting: session_id={}", session_id);
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
                                "Receive failed with error: session_id={}, error=0x{:08X}",
                                session_id,
                                error_code);
                        }
                        co_return;
                    }

                    // Successfully received message
                    auto&& [header, body] = std::get<1>(result);

                    if (header.Raw().message_type
                        == static_cast<uint8_t>(MessageType::RESPONSE))
                    {
                        CompletePendingCall(header.Raw().call_id, DAS_S_OK, std::move(body));
                    }
                    else
                    {
                        co_await DispatchToHandlerCoroutine(header.Raw(), body, *transport);
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
                            e.what());
                        co_return;
                    }
                }
                catch (const std::exception& e)
                {
                    DAS_CORE_LOG_ERROR(
                        "Receive failed with exception: session_id={}, error={}",
                        session_id,
                        e.what());
                    co_return;
                }

                // Handle retry outside catch block (co_await not allowed in catch)
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
