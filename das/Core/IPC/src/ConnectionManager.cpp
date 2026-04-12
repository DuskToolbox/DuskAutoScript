#include <atomic>
#include <chrono>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/Logger/Logger.h>
#include <shared_mutex>
#include <unordered_map>

#include <das/Core/IPC/DefaultAsyncIpcTransport.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>

DAS_CORE_IPC_NS_BEGIN

struct ConnectionManager::Impl
{
    std::unordered_map<uint16_t, ConnectionInfo> connections_;
    // 存储 HostLauncher 的引用
    std::unordered_map<uint16_t, DasPtr<HostLauncher>> host_launchers_;
    // 直接传输层注册（用于 Host 模式，无需 HostLauncher）
    std::unordered_map<uint16_t, DefaultAsyncIpcTransport*> direct_transports_;
    // 共享内存池（每个连接一个，按 remote_id 索引）
    std::unordered_map<uint16_t, std::unique_ptr<SharedMemoryPool>> shm_pools_;
    mutable std::shared_mutex connections_mutex_;
    std::atomic<bool>         running_{false};
    std::thread               heartbeat_thread_;
    uint16_t                  local_id_{0};
};

ConnectionManager::ConnectionManager(uint16_t local_id)
    : impl_(std::make_unique<Impl>())
{
    impl_->local_id_ = local_id;
}

ConnectionManager::~ConnectionManager()
{
    StopHeartbeatThread();

    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);
    for (auto& [remote_id, info] : impl_->connections_)
    {
        CleanupConnectionResources(remote_id);
    }
    impl_->connections_.clear();
    impl_->host_launchers_.clear();
    impl_->direct_transports_.clear();
}

DasResult ConnectionManager::RegisterConnection(
    uint16_t remote_id,
    uint16_t local_id)
{
    ConnectionInfo info{};
    info.host_id = remote_id;
    info.plugin_id = local_id;
    info.is_alive = true;
    info.last_heartbeat_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    info.launcher = nullptr;
    info.shm_pool = nullptr;

    // 创建 per-connection SharedMemoryPool（deterministic name from session
    // IDs） MainProcess side: local_id_ = main, remote_id = host
    std::string shm_name = "das_shm_" + std::to_string(impl_->local_id_) + "_"
                           + std::to_string(remote_id);
    constexpr size_t SHM_POOL_INITIAL_SIZE = 4 * 1024 * 1024; // 4MB
    try
    {
        auto pool =
            std::make_unique<SharedMemoryPool>(shm_name, SHM_POOL_INITIAL_SIZE);
        info.shm_pool = pool.get();
        impl_->shm_pools_[remote_id] = std::move(pool);
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_WARN(
            "Failed to create SHM pool for session {}: {}",
            remote_id,
            e.what());
        info.shm_pool = nullptr; // Fallback: SHM unavailable, pipe-only
    }

    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);
    impl_->connections_[remote_id] = std::move(info);

    return DAS_S_OK;
}

DasResult ConnectionManager::UnregisterConnection(uint16_t remote_id)
{
    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);
    auto it = impl_->connections_.find(remote_id);
    if (it == impl_->connections_.end())
    {
        DAS_CORE_LOG_ERROR(
            "Connection not found for remote_id = {}",
            remote_id);
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    CleanupConnectionResources(remote_id);
    impl_->connections_.erase(it);

    return DAS_S_OK;
}

DasResult ConnectionManager::SendHeartbeat(uint16_t remote_id)
{
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);
    auto it = impl_->connections_.find(remote_id);
    if (it == impl_->connections_.end())
    {
        DAS_CORE_LOG_ERROR(
            "Connection not found for remote_id = {}",
            remote_id);
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    it->second.last_heartbeat_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    return DAS_S_OK;
}

bool ConnectionManager::IsConnectionAlive(uint16_t remote_id) const
{
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);
    auto it = impl_->connections_.find(remote_id);
    if (it == impl_->connections_.end())
    {
        DAS_CORE_LOG_WARN("Connection not found for remote_id = {}", remote_id);
        return false;
    }
    return it->second.is_alive;
}

DasResult ConnectionManager::GetConnection(
    uint16_t        session_id,
    ConnectionInfo& out_info) const
{
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    auto it = impl_->connections_.find(session_id);
    if (it == impl_->connections_.end())
    {
        DAS_CORE_LOG_ERROR(
            "Connection not found for session_id = {}",
            session_id);
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    // Shallow copy: 只复制非 DasPtr 字段
    out_info.host_id = it->second.host_id;
    out_info.plugin_id = it->second.plugin_id;
    out_info.is_alive = it->second.is_alive;
    out_info.last_heartbeat_ms = it->second.last_heartbeat_ms;
    out_info.launcher =
        nullptr; // DasPtr 字段不复制，调用者应使用 GetLauncher()
    out_info.shm_pool = it->second.shm_pool;
    return DAS_S_OK;
}

DasResult ConnectionManager::RegisterHostLauncher(
    uint16_t             session_id,
    DasPtr<HostLauncher> launcher)
{
    if (!launcher)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    // 存储到 host_launchers_ map
    impl_->host_launchers_[session_id] = launcher;

    // 如果连接已存在，更新信息（shm_pool 已由 RegisterConnection 设置）
    auto it = impl_->connections_.find(session_id);
    if (it != impl_->connections_.end())
    {
        it->second.launcher = launcher;
        it->second.is_alive = true;
        it->second.last_heartbeat_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
    else
    {
        // 创建 ConnectionInfo（MainProcess side: 创建 SHM pool）
        ConnectionInfo info{};
        info.host_id = session_id;
        info.plugin_id = 0;
        info.is_alive = true;
        info.last_heartbeat_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        info.launcher = launcher;
        info.shm_pool = nullptr;

        // 创建 per-connection SharedMemoryPool（deterministic name from session
        // IDs）
        std::string shm_name = "das_shm_" + std::to_string(impl_->local_id_)
                               + "_" + std::to_string(session_id);
        constexpr size_t SHM_POOL_INITIAL_SIZE = 4 * 1024 * 1024; // 4MB
        try
        {
            auto pool = std::make_unique<SharedMemoryPool>(
                shm_name,
                SHM_POOL_INITIAL_SIZE);
            info.shm_pool = pool.get();
            impl_->shm_pools_[session_id] = std::move(pool);
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_WARN(
                "Failed to create SHM pool for session {}: {}",
                session_id,
                e.what());
            info.shm_pool = nullptr;
        }

        impl_->connections_[session_id] = std::move(info);
    }

    DAS_CORE_LOG_INFO("HostLauncher registered: session_id={}", session_id);
    return DAS_S_OK;
}

DasResult ConnectionManager::UnregisterHostLauncher(uint16_t session_id)
{
    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    auto it = impl_->host_launchers_.find(session_id);
    if (it == impl_->host_launchers_.end())
    {
        DAS_CORE_LOG_ERROR(
            "HostLauncher not found for session_id = {}",
            session_id);
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    impl_->host_launchers_.erase(it);
    impl_->connections_.erase(session_id);

    DAS_CORE_LOG_INFO("HostLauncher unregistered: session_id={}", session_id);
    return DAS_S_OK;
}

DasResult ConnectionManager::RegisterTransport(
    uint16_t                  session_id,
    DefaultAsyncIpcTransport* transport)
{
    if (!transport)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    // 直接存储到 direct_transports_ map
    impl_->direct_transports_[session_id] = transport;

    // Host side: 打开已存在的 SHM pool（deterministic name）
    // On Host side: main_session_id = remote_id, host_session_id = local_id_
    std::string       shm_name = "das_shm_" + std::to_string(session_id) + "_"
                                 + std::to_string(impl_->local_id_);
    SharedMemoryPool* pool_ptr = nullptr;
    try
    {
        auto pool = SharedMemoryPool::Open(shm_name);
        pool_ptr = pool.get();
        impl_->shm_pools_[session_id] = std::move(pool);
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_WARN(
            "Failed to open SHM pool for session {}: {}",
            session_id,
            e.what());
        pool_ptr = nullptr;
    }

    // 同时创建/更新 ConnectionInfo 保持一致性
    auto it = impl_->connections_.find(session_id);
    if (it != impl_->connections_.end())
    {
        it->second.is_alive = true;
        it->second.last_heartbeat_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        it->second.shm_pool = pool_ptr;
    }
    else
    {
        ConnectionInfo info{};
        info.host_id = session_id;
        info.plugin_id = 0;
        info.is_alive = true;
        info.last_heartbeat_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        info.launcher = nullptr;
        info.shm_pool = pool_ptr;
        impl_->connections_[session_id] = std::move(info);
    }

    DAS_CORE_LOG_INFO(
        "Transport registered directly: session_id={}",
        session_id);
    return DAS_S_OK;
}

DasPtr<HostLauncher> ConnectionManager::GetLauncher(uint16_t session_id) const
{
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    auto it = impl_->host_launchers_.find(session_id);
    if (it == impl_->host_launchers_.end())
    {
        return nullptr;
    }
    return it->second;
}

DefaultAsyncIpcTransport* ConnectionManager::GetTransport(
    uint16_t session_id) const
{
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    // 首先检查直接传输层注册（Host 模式）
    auto direct_it = impl_->direct_transports_.find(session_id);
    if (direct_it != impl_->direct_transports_.end())
    {
        return direct_it->second;
    }

    // 从 host_launchers_ 获取 launcher
    auto launcher_it = impl_->host_launchers_.find(session_id);
    if (launcher_it == impl_->host_launchers_.end())
    {
        // 检查 connections_（兼容路径）
        auto conn_it = impl_->connections_.find(session_id);
        if (conn_it == impl_->connections_.end())
        {
            DAS_CORE_LOG_WARN(
                "Connection not found for session_id = {}",
                session_id);
            return nullptr;
        }

        // 如果有 launcher，从 launcher 获取 transport
        if (conn_it->second.launcher)
        {
            return conn_it->second.launcher->GetTransport();
        }
        return nullptr;
    }

    // 从 HostLauncher 获取 Transport
    return launcher_it->second->GetTransport();
}

DasResult ConnectionManager::SetConnectionAlive(
    uint16_t session_id,
    bool     is_alive)
{
    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    auto it = impl_->connections_.find(session_id);
    if (it == impl_->connections_.end())
    {
        DAS_CORE_LOG_ERROR(
            "Connection not found for session_id = {}",
            session_id);
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    it->second.is_alive = is_alive;
    if (is_alive)
    {
        it->second.last_heartbeat_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
    }

    return DAS_S_OK;
}

std::vector<uint16_t> ConnectionManager::GetConnectedSessions() const
{
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    std::vector<uint16_t> sessions;
    sessions.reserve(impl_->connections_.size());

    for (const auto& [session_id, info] : impl_->connections_)
    {
        // Intentionally filters out Host-mode connections; only returns
        // sessions managed via HostLauncher
        if (info.is_alive && info.launcher)
        {
            sessions.push_back(session_id);
        }
    }

    return sessions;
}

void ConnectionManager::StartHeartbeatThread()
{
    if (impl_->running_.load())
    {
        return;
    }

    impl_->running_.store(true);
    impl_->heartbeat_thread_ = std::thread(
        [this]()
        {
            uint64_t interval_ms = HEARTBEAT_INTERVAL_MS;
            uint64_t timeout_ms = HEARTBEAT_TIMEOUT_MS;

            while (impl_->running_.load())
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(interval_ms));

                if (!impl_->running_.load())
                {
                    break;
                }

                // 1. 发送心跳消息
                SendHeartbeatToAll();

                // 2. 检查超时并清理
                uint64_t current_time_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());

                std::unique_lock<std::shared_mutex> lock(
                    impl_->connections_mutex_);

                for (auto it = impl_->connections_.begin();
                     it != impl_->connections_.end();)
                {
                    uint64_t elapsed_ms =
                        current_time_ms - it->second.last_heartbeat_ms;

                    if (elapsed_ms > timeout_ms)
                    {
                        it->second.is_alive = false;

                        // 获取 session_id 用于清理 host_launchers_
                        uint16_t session_id = it->first;

                        // 从 host_launchers_ 中移除（触发 DasPtr 释放 ->
                        // 引用计数归零 -> HostLauncher 析构）
                        impl_->host_launchers_.erase(session_id);

                        CleanupConnectionResources(it->first);
                        it = impl_->connections_.erase(it);

                        DAS_CORE_LOG_WARN(
                            "Connection timed out, cleaning up: session_id={}",
                            session_id);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
        });
}

void ConnectionManager::StopHeartbeatThread()
{
    if (!impl_->running_.load())
    {
        return;
    }

    impl_->running_.store(false);
    if (impl_->heartbeat_thread_.joinable())
    {
        impl_->heartbeat_thread_.join();
    }
}

DasResult ConnectionManager::SendHeartbeatToAll()
{
    auto sessions = GetConnectedSessions();

    // V3: 获取本地 session_id
    uint16_t local_session_id = impl_->local_id_;

    for (uint16_t session_id : sessions)
    {
        auto* transport = GetTransport(session_id);
        if (!transport || !transport->IsConnected())
        {
            // 跳过无效连接
            continue;
        }

        // 构建心跳消息
        HeartbeatV1 heartbeat;
        InitHeartbeat(
            heartbeat,
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()));

        auto validated_header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::REQUEST)
                .SetControlPlaneCommand(
                    HandshakeInterfaceId::HANDSHAKE_IFACE_HEARTBEAT)
                .SetBodySize(sizeof(heartbeat))
                .SetSourceSessionId(local_session_id)
                .SetTargetSessionId(session_id)
                .Build();

        // 将心跳发送操作 post 到 transport 所属的 io_context
        // 这样心跳发送和正常消息处理都在同一个 io_context
        // 上，避免跨线程并发问题
        boost::asio::post(
            transport->GetIoContext(),
            [transport,
             header = validated_header,
             heartbeat_copy = heartbeat]() mutable
            {
                // 使用协程异步发送心跳
                boost::asio::co_spawn(
                    transport->GetIoContext(),
                    [transport,
                     header,
                     heartbeat_copy]() mutable -> boost::asio::awaitable<void>
                    {
                        auto result = co_await transport->SendCoroutine(
                            header,
                            reinterpret_cast<const uint8_t*>(&heartbeat_copy),
                            sizeof(heartbeat_copy));
                        if (result != DAS_S_OK)
                        {
                            DAS_CORE_LOG_WARN(
                                "Heartbeat send failed for session, error={}",
                                result);
                        }
                    },
                    boost::asio::detached);
            });
    }

    return DAS_S_OK;
}

void ConnectionManager::UpdateHeartbeatTimestamp(uint16_t session_id)
{
    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    auto it = impl_->connections_.find(session_id);
    if (it != impl_->connections_.end())
    {
        it->second.last_heartbeat_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
}

boost::asio::awaitable<DasResult> ConnectionManager::ForwardMessage(
    uint16_t                         target_session_id,
    const ValidatedIPCMessageHeader& header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    // 1. 查找目标 Transport
    auto* transport = GetTransport(target_session_id);
    if (!transport)
    {
        DAS_CORE_LOG_ERROR(
            "Forward failed: target_session_id={} not found",
            target_session_id);
        co_return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    // 2. 获取本地 session_id
    uint16_t local_session_id = impl_->local_id_;

    // 3. 使用 IPCMessageHeaderBuilder 重建 header，修改 source_session_id
    auto forward_header = IPCMessageHeaderBuilder()
                              .SetMessageType(header.GetMessageType())
                              .SetInterfaceId(header.GetInterfaceId())
                              .SetCallId(header.GetCallId())
                              .SetFlags(header.GetFlags())
                              .SetBodySize(static_cast<uint32_t>(body_size))
                              .SetSourceSessionId(local_session_id)
                              .SetTargetSessionId(target_session_id)
                              .Build();

    // 4. 发送消息
    auto result =
        co_await transport->SendCoroutine(forward_header, body, body_size);

    if (result != DAS_S_OK)
    {
        DAS_CORE_LOG_ERROR(
            "Forward failed: target_session_id={}, error={}",
            target_session_id,
            result);
    }
    else
    {
        DAS_CORE_LOG_DEBUG(
            "Forwarded message to target_session_id={}, message_type={}",
            target_session_id,
            static_cast<int>(header.GetMessageType()));
    }

    co_return result;
}

DasResult ConnectionManager::CleanupConnectionResources(uint16_t remote_id)
{
    // 清理直接传输层
    impl_->direct_transports_.erase(remote_id);

    // 清理 SHM pool（unique_ptr 自动析构）
    impl_->shm_pools_.erase(remote_id);

    auto it = impl_->connections_.find(remote_id);
    if (it == impl_->connections_.end())
    {
        DAS_CORE_LOG_ERROR(
            "Connection not found for remote_id = {}",
            remote_id);
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    ConnectionInfo& info = it->second;

    // 清理顺序：launcher 释放 -> HostLauncher 析构 -> 自动清理
    // 注意：B3.1 规范 - Child 仅释放引用，不删除资源（Host 负责删除）

    info.shm_pool = nullptr;
    info.launcher = nullptr;
    info.is_alive = false;

    return DAS_S_OK;
}

void ConnectionManager::CleanupAllStaleBlocks()
{
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    for (const auto& [session_id, conn_info] : impl_->connections_)
    {
        (void)session_id;
        if (conn_info.shm_pool)
        {
            conn_info.shm_pool->CleanupStaleBlocks();
        }
    }
}

DAS_CORE_IPC_NS_END
