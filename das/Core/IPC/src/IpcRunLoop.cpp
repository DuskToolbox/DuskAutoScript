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
#include <stdexec/execution.hpp>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/asio.hpp>

#ifdef _WIN32
#include <das/Core/IPC/Win32AsyncIpcTransport.h>
#else
#include <das/Core/IPC/UnixAsyncIpcTransport.h>
#endif

#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

IpcRunLoop::IpcRunLoop() = default;

IpcRunLoop::~IpcRunLoop() { RequestStop(); }

DasResult IpcRunLoop::Initialize()
{
    io_context_ = std::make_unique<boost::asio::io_context>();
    async_transport_ = std::make_unique<DefaultAsyncIpcTransport>(*io_context_);
    timeout_timer_ = std::make_unique<boost::asio::steady_timer>(*io_context_);

    // Create ConnectionManager for MainProcess mode
    // In Host mode, connection_manager_ remains nullptr (no heartbeat needed)
    connection_manager_ = std::make_unique<ConnectionManager>();

    return DAS_S_OK;
}

DasResult IpcRunLoop::Initialize(
    const std::string& read_queue_name,
    const std::string& write_queue_name,
    bool               is_server)
{
    io_context_ = std::make_unique<boost::asio::io_context>();
    async_transport_ = std::make_unique<DefaultAsyncIpcTransport>(*io_context_);
    timeout_timer_ = std::make_unique<boost::asio::steady_timer>(*io_context_);

    DasResult result = DAS_S_OK;

    if (is_server)
    {
        result = async_transport_->Initialize(
            read_queue_name,
            write_queue_name,
            true);
        if (DAS::IsFailed(result))
        {
            DAS_CORE_LOG_ERROR(
                "Failed to initialize transport: read={}, write={}",
                read_queue_name,
                write_queue_name);
            return result;
        }
    }
    else
    {
        result = async_transport_->Connect(read_queue_name, write_queue_name);
        if (DAS::IsFailed(result))
        {
            DAS_CORE_LOG_ERROR(
                "Failed to connect transport: read={}, write={}",
                read_queue_name,
                write_queue_name);
            return result;
        }
    }
    return DAS_S_OK;
}

DasResult IpcRunLoop::Shutdown()
{
    RequestStop();

    // 清理 ConnectionManager
    if (connection_manager_)
    {
        connection_manager_->StopHeartbeatThread();
        connection_manager_->Shutdown();
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
    async_transport_.reset();
    io_context_.reset();
    return DAS_S_OK;
}

void IpcRunLoop::RequestStop()
{
    if (!running_.load())
    {
        return;
    }

    running_.store(false);

    // 1. 先关闭 transport，让协程自然退出
    if (async_transport_)
    {
        async_transport_->Close();
    }

    // 2. 重置 work guard，允许 io_context_->run() 返回
    work_guard_.reset();

    // 3. 取消所有 pending calls
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

IMessageHandler* IpcRunLoop::GetHandler(uint32_t interface_id) const
{
    auto it = handlers_.find(interface_id);
    if (it != handlers_.end())
    {
        return it->second.get();
    }
    return nullptr;
}

DasResult IpcRunLoop::DispatchToHandler(
    const IPCMessageHeader&     header,
    const std::vector<uint8_t>& body)
{
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
            return DAS_S_OK;
        }
        // REQUEST 继续交给 handler 处理
    }

    auto* handler = GetHandler(header.interface_id);
    if (handler)
    {
        IpcResponseSender sender(*this);
        // 同步版本使用 co_spawn + use_future 调用协程（用于非协程上下文）
        // 注意：在 io_context 线程中调用会导致死锁，仅用于非 io_context 线程
        try
        {
            auto future = boost::asio::co_spawn(
                GetIoContext(),
                handler->HandleMessage(header, body, sender),
                boost::asio::use_future);
            return future.get();
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_ERROR("DispatchToHandler failed: {}", e.what());
            return DAS_E_FAIL;
        }
    }

    DAS_CORE_LOG_WARN(
        "No handler found for interface_id = {}",
        header.interface_id);
    return DAS_E_NOT_FOUND;
}

boost::asio::awaitable<void> IpcRunLoop::DispatchToHandlerCoroutine(
    const IPCMessageHeader&     header,
    const std::vector<uint8_t>& body)
{
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

    auto* handler = GetHandler(header.interface_id);
    if (handler)
    {
        IpcResponseSender sender(*this);
        auto result = co_await handler->HandleMessage(header, body, sender);
        if (DAS::IsFailed(result))
        {
            DAS_CORE_LOG_WARN("Handler returned error: 0x{:08X}", result);
        }
    }
    else
    {
        DAS_CORE_LOG_WARN(
            "No handler found for interface_id = {}",
            header.interface_id);
    }
}

bool IpcRunLoop::ReceiveAndDispatch(std::chrono::milliseconds timeout)
{
    (void)timeout;

    if (!async_transport_)
    {
        return false;
    }

    auto result_sender = async_transport_->Receive();
    auto result_opt = stdexec::sync_wait(std::move(result_sender));

    if (!result_opt.has_value())
    {
        return false;
    }

    auto&& [ec, result_variant] = std::move(*result_opt);

    if (ec)
    {
        try
        {
            std::rethrow_exception(ec);
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_ERROR("Receive failed: {}", e.what());
        }
        return false;
    }

    if (result_variant.index() == 0)
    {
        DasResult error_code = std::get<0>(result_variant);
        DAS_CORE_LOG_ERROR("Receive failed with error: {}", error_code);
        return false;
    }

    auto&& [header, body] = std::get<1>(result_variant);

    if (header.Raw().message_type
        == static_cast<uint8_t>(MessageType::RESPONSE))
    {
        CompletePendingCall(header.Raw().call_id, DAS_S_OK, std::move(body));
        return true;
    }

    DispatchToHandler(header.Raw(), body);
    return true;
}

std::pair<DasResult, uint64_t> IpcRunLoop::PrepareSendRequest(
    const ValidatedIPCMessageHeader& request_header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    if (!async_transport_)
    {
        return {DAS_E_IPC_NOT_INITIALIZED, 0};
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

    auto send_sender =
        async_transport_->Send(validated_header, body, body_size);
    auto result_opt = stdexec::sync_wait(std::move(send_sender));

    if (!result_opt.has_value())
    {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_.erase(call_id);
        return {DAS_E_IPC_CONNECTION_LOST, 0};
    }

    auto&& [ec, send_result] = std::move(*result_opt);

    if (ec)
    {
        try
        {
            std::rethrow_exception(ec);
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_ERROR("Send failed: {}", e.what());
        }
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_.erase(call_id);
        return {DAS_E_IPC_CONNECTION_LOST, 0};
    }

    return {send_result, call_id};
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

    auto send_sender = transport->Send(validated_header, body, body_size);
    auto result_opt = stdexec::sync_wait(std::move(send_sender));

    if (!result_opt.has_value())
    {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_.erase(call_id);
        return {DAS_E_IPC_CONNECTION_LOST, 0};
    }

    auto&& [ec, send_result] = std::move(*result_opt);

    if (ec)
    {
        try
        {
            std::rethrow_exception(ec);
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_ERROR("Send failed: {}", e.what());
        }
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_calls_.erase(call_id);
        return {DAS_E_IPC_CONNECTION_LOST, 0};
    }

    return {send_result, call_id};
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

DasResult IpcRunLoop::SendResponse(
    const ValidatedIPCMessageHeader& response_header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    if (!async_transport_)
    {
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    // 检查 transport 是否已连接，避免 sync_wait 永久阻塞
    if (!async_transport_->IsConnected())
    {
        return DAS_E_IPC_NO_CONNECTIONS;
    }

    auto sender = async_transport_->Send(response_header, body, body_size);
    auto result_opt = stdexec::sync_wait(std::move(sender));

    if (!result_opt.has_value())
    {
        return DAS_E_IPC_CONNECTION_LOST;
    }

    auto&& [ec, send_result] = std::move(*result_opt);

    if (ec)
    {
        try
        {
            std::rethrow_exception(ec);
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_ERROR("Send failed: {}", e.what());
        }
        return DAS_E_IPC_CONNECTION_LOST;
    }

    return send_result;
}

boost::asio::awaitable<DasResult> IpcRunLoop::SendResponseCoroutine(
    const ValidatedIPCMessageHeader& response_header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    if (!async_transport_)
    {
        co_return DAS_E_IPC_NOT_INITIALIZED;
    }

    if (!async_transport_->IsConnected())
    {
        co_return DAS_E_IPC_NO_CONNECTIONS;
    }

    try
    {
        auto result = co_await async_transport_->SendCoroutine(response_header, body, body_size);
        co_return result;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR("SendResponseCoroutine failed: {}", e.what());
        co_return DAS_E_IPC_CONNECTION_LOST;
    }
}

DasResult IpcRunLoop::SendEvent(
    const ValidatedIPCMessageHeader& event_header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    if (!async_transport_)
    {
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    // 检查 transport 是否已连接
    if (!async_transport_->IsConnected())
    {
        return DAS_E_IPC_NO_CONNECTIONS;
    }

    // fire-and-forget: 派生协程发送，不等待完成
    // 复制数据以确保协程执行时数据仍然有效
    auto validated_header_copy = event_header;
    std::vector<uint8_t> body_copy(body, body + body_size);

    boost::asio::co_spawn(
        *io_context_,
        [this, validated_header_copy, body_copy = std::move(body_copy)]()
            -> boost::asio::awaitable<void>
        {
            try
            {
                auto result = co_await SendEventCoroutine(
                    validated_header_copy,
                    body_copy.data(),
                    body_copy.size());
                if (DAS::IsFailed(result))
                {
                    DAS_CORE_LOG_ERROR(
                        "SendEvent fire-and-forget failed: 0x{:08X}",
                        result);
                }
            }
            catch (const std::exception& e)
            {
                DAS_CORE_LOG_ERROR("SendEvent fire-and-forget exception: {}", e.what());
            }
        },
        boost::asio::detached);

    return DAS_S_OK;
}

boost::asio::awaitable<DasResult> IpcRunLoop::SendEventCoroutine(
    const ValidatedIPCMessageHeader& event_header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    if (!async_transport_)
    {
        co_return DAS_E_IPC_NOT_INITIALIZED;
    }

    if (!async_transport_->IsConnected())
    {
        co_return DAS_E_IPC_NO_CONNECTIONS;
    }

    const IPCMessageHeader& raw = event_header.Raw();
    auto                    validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::EVENT)
            .SetBusinessInterface(raw.interface_id, raw.method_id)
            .SetBodySize(raw.body_size)
            .SetCallId(raw.call_id)
            .SetObject(raw.session_id, raw.generation, raw.local_id)
            .Build();

    try
    {
        auto result = co_await async_transport_->SendCoroutine(validated_header, body, body_size);
        co_return result;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR("SendEventCoroutine failed: {}", e.what());
        co_return DAS_E_IPC_CONNECTION_LOST;
    }
}

bool IpcRunLoop::IsRunning() const { return running_.load(); }

void IpcRunLoop::StartAsyncReceive()
{
    if (!io_context_ || !async_transport_ || !running_.load())
    {
        return;
    }

    // 检查 transport 是否已连接
    // 注意：未注册的 transport 从未连接，不应该启动接收循环
    // 对于 Host 端（服务端），创建命名管道后 IsConnected() 返回 true
    if (!async_transport_->IsConnected())
    {
        DAS_CORE_LOG_DEBUG(
            "StartAsyncReceive: transport not connected, skipping receive loop");
        return;
    }

    // 使用 boost::asio::co_spawn 在 io_context 上异步运行接收协程
    // 这样不会阻塞 io_context 线程，实现真正的事件驱动
    boost::asio::co_spawn(
        *io_context_,
        [this]() -> boost::asio::awaitable<void>
        {
            while (running_.load())
            {
                // 用于标记是否需要等待客户端连接
                bool need_wait_for_client = false;

                try
                {
                    // 直接使用 transport 的协程接口
                    auto result = co_await async_transport_->ReceiveCoroutine();

                    if (!running_.load())
                    {
                        co_return;
                    }

                    if (result.index() == 0)
                    {
                        // 错误情况
                        DasResult error_code = std::get<0>(result);
                        if (error_code != DAS_S_OK)
                        {
                            DAS_CORE_LOG_ERROR("Receive failed with error: {}", error_code);
                        }
                        co_return;
                    }

                    // 成功接收消息
                    auto&& [header, body] = std::get<1>(result);

                    if (header.Raw().message_type == static_cast<uint8_t>(MessageType::RESPONSE))
                    {
                        CompletePendingCall(header.Raw().call_id, DAS_S_OK, std::move(body));
                    }
                    else
                    {
                        co_await DispatchToHandlerCoroutine(header.Raw(), body);
                    }
                }
                catch (const boost::system::system_error& e)
                {
                    // 当 Close() 被调用时，async_read 会抛出 operation_aborted 异常
                    if (e.code() == boost::asio::error::operation_aborted)
                    {
                        DAS_CORE_LOG_DEBUG("Receive loop stopped - operation aborted");
                        // 正常退出，不设置 running_ = false，由 RequestStop() 负责
                        co_return;
                    }
                    else if (e.code() == boost::asio::error::eof)
                    {
                        // 管道关闭或无数据可读，这是正常情况（如对方进程退出）
                        DAS_CORE_LOG_DEBUG("Receive loop stopped - EOF reached");
                        co_return;
                    }
                    else if (e.code().value() == 536)
                    {
                        // ERROR_NO_DATA (536): 等待打开管道另一端的进程
                        // 这是服务端命名管道的正常情况 - 客户端尚未连接
                        need_wait_for_client = true;
                    }
                    else
                    {
                        DAS_CORE_LOG_ERROR("Receive failed with system error: {}", e.what());
                        // 非正常错误，直接退出循环，但不改变 running_ 状态
                        // 让 io_context_->run() 因为没有更多工作而返回
                        co_return;
                    }
                }
                catch (const std::exception& e)
                {
                    DAS_CORE_LOG_ERROR("Receive failed with exception: {}", e.what());
                    // 异常退出，不改变 running_ 状态
                    co_return;
                }

                // 在 catch 块外处理等待客户端连接的情况
                if (need_wait_for_client)
                {
                    DAS_CORE_LOG_DEBUG("Waiting for client to connect to pipe...");
                    auto timer = boost::asio::steady_timer(*io_context_);
                    timer.expires_after(std::chrono::milliseconds(100));
                    co_await timer.async_wait(boost::asio::use_awaitable);
                    // continue 循环重试接收
                }
            }
        },
        boost::asio::detached);
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
    work_guard_ = std::make_unique<WorkGuard>(io_context_->get_executor());

    // 启动心跳检测线程（MainProcess 模式）
    if (connection_manager_)
    {
        connection_manager_->StartHeartbeatThread();
    }

    // 启动异步接收链
    StartAsyncReceive();

    // 启动超时检查定时器
    ScheduleTimeoutCheck();

    // 阻塞运行事件循环
    io_context_->run();

    return exit_code_.load();
}

DasResult IpcRunLoop::ProcessMessage(
    const IPCMessageHeader& header,
    const uint8_t*          body,
    size_t                  body_size)
{
    if (header.message_type == static_cast<uint8_t>(MessageType::RESPONSE))
    {
        std::vector<uint8_t> response_body(body, body + body_size);
        CompletePendingCall(header.call_id, DAS_S_OK, std::move(response_body));
        return DAS_S_OK;
    }

    if (header.message_type == static_cast<uint8_t>(MessageType::REQUEST))
    {
        std::vector<uint8_t> body_vec(body, body + body_size);
        auto dispatch_result = DispatchToHandler(header, body_vec);

        if (dispatch_result != DAS_E_NOT_FOUND)
        {
            return dispatch_result;
        }

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

        return SendResponse(validated_response, nullptr, 0);
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
                        co_await DispatchToHandlerCoroutine(header.Raw(), body);
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
