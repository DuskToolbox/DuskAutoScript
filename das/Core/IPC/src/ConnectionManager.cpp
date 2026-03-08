#include <atomic>
#include <chrono>

#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/Logger/Logger.h>
#include <shared_mutex>
#include <unordered_map>

#ifdef _WIN32
#include <das/Core/IPC/Win32AsyncIpcTransport.h>
#else
#include <das/Core/IPC/UnixAsyncIpcTransport.h>
#endif

DAS_CORE_IPC_NS_BEGIN

struct ConnectionManager::Impl
{
    std::unordered_map<uint16_t, ConnectionInfo> connections_;
    // 存储 HostLauncher 的引用
    std::unordered_map<uint16_t, DasPtr<IHostLauncher>> host_launchers_;
    mutable std::shared_mutex                            connections_mutex_;
    std::atomic<bool>                                    running_{false};
    std::thread                                          heartbeat_thread_;
    uint16_t                                             local_id_{0};
};

ConnectionManager::ConnectionManager() : impl_(std::make_unique<Impl>()) {}

ConnectionManager::~ConnectionManager()
{
    StopHeartbeatThread();
    Shutdown();
}

DasResult ConnectionManager::Initialize(uint16_t local_id)
{
    impl_->local_id_ = local_id;
    return DAS_S_OK;
}

DasResult ConnectionManager::Shutdown()
{
    StopHeartbeatThread();

    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);
    for (auto& [remote_id, info] : impl_->connections_)
    {
        CleanupConnectionResources(remote_id, impl_->local_id_);
    }
    impl_->connections_.clear();
    impl_->host_launchers_.clear();

    return DAS_S_OK;
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

    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);
    impl_->connections_[remote_id] = std::move(info);

    return DAS_S_OK;
}

DasResult ConnectionManager::UnregisterConnection(
    uint16_t remote_id,
    uint16_t local_id)
{
    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);
    auto it = impl_->connections_.find(remote_id);
    if (it == impl_->connections_.end())
    {
        DAS_CORE_LOG_ERROR("Connection not found for remote_id = {}", remote_id);
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    CleanupConnectionResources(remote_id, local_id);
    impl_->connections_.erase(it);

    return DAS_S_OK;
}

DasResult ConnectionManager::SendHeartbeat(uint16_t remote_id)
{
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);
    auto it = impl_->connections_.find(remote_id);
    if (it == impl_->connections_.end())
    {
        DAS_CORE_LOG_ERROR("Connection not found for remote_id = {}", remote_id);
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
    static_cast<void>(out_info); // 暂时不使用
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    auto it = impl_->connections_.find(session_id);
    if (it == impl_->connections_.end())
    {
        DAS_CORE_LOG_ERROR("Connection not found for session_id = {}", session_id);
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    // 注意：由于 ConnectionInfo 包含 DasPtr，无法拷贝
    // 调用者应使用 GetLauncher() 或 GetTransport() 获取指针
    return DAS_E_FAIL;
}

DasResult ConnectionManager::RegisterHostLauncher(
    uint16_t                 session_id,
    DasPtr<IHostLauncher>   launcher)
{
    if (!launcher)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    // 存储到 host_launchers_ map
    impl_->host_launchers_[session_id] = launcher;

    // 创建/更新 ConnectionInfo
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

    // 如果连接已存在，更新信息
    auto it = impl_->connections_.find(session_id);
    if (it != impl_->connections_.end())
    {
        it->second.launcher = launcher;
        it->second.is_alive = true;
        it->second.last_heartbeat_ms = info.last_heartbeat_ms;
    }
    else
    {
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
        DAS_CORE_LOG_ERROR("HostLauncher not found for session_id = {}", session_id);
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    impl_->host_launchers_.erase(it);
    impl_->connections_.erase(session_id);

    DAS_CORE_LOG_INFO("HostLauncher unregistered: session_id={}", session_id);
    return DAS_S_OK;
}

DasPtr<IHostLauncher> ConnectionManager::GetLauncher(uint16_t session_id) const
{
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    auto it = impl_->host_launchers_.find(session_id);
    if (it == impl_->host_launchers_.end())
    {
        return nullptr;
    }
    return it->second;
}

DefaultAsyncIpcTransport* ConnectionManager::GetTransport(uint16_t session_id) const
{
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    // 从 host_launchers_ 获取 launcher
    auto launcher_it = impl_->host_launchers_.find(session_id);
    if (launcher_it == impl_->host_launchers_.end())
    {
        // 检查 connections_（兼容路径）
        auto conn_it = impl_->connections_.find(session_id);
        if (conn_it == impl_->connections_.end())
        {
            DAS_CORE_LOG_WARN("Connection not found for session_id = {}", session_id);
            return nullptr;
        }

        // 如果有 launcher，从 launcher 获取 transport
        if (conn_it->second.launcher)
        {
            auto* concrete = dynamic_cast<HostLauncher*>(conn_it->second.launcher.Get());
            if (concrete)
            {
                return concrete->GetTransport();
            }
        }
        return nullptr;
    }

    // 从 HostLauncher 获取 Transport
    auto* concrete = dynamic_cast<HostLauncher*>(launcher_it->second.Get());
    if (!concrete)
    {
        DAS_CORE_LOG_WARN("Failed to cast IHostLauncher to HostLauncher for session_id = {}", session_id);
        return nullptr;
    }

    return concrete->GetTransport();
}

DasResult ConnectionManager::SetConnectionAlive(
    uint16_t session_id,
    bool     is_alive)
{
    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    auto it = impl_->connections_.find(session_id);
    if (it == impl_->connections_.end())
    {
        DAS_CORE_LOG_ERROR("Connection not found for session_id = {}", session_id);
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

                uint64_t current_time_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();

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

                        // 从 host_launchers_ 中移除（触发 DasPtr 释放 -> 引用计数归零 -> HostLauncher 析构）
                        impl_->host_launchers_.erase(session_id);

                        CleanupConnectionResources(it->first, impl_->local_id_);
                        it = impl_->connections_.erase(it);

                        DAS_CORE_LOG_INFO("Connection timed out, cleaned up: session_id={}", session_id);
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

DasResult ConnectionManager::CleanupConnectionResources(
    uint16_t remote_id,
    uint16_t local_id)
{
    (void)local_id;

    auto it = impl_->connections_.find(remote_id);
    if (it == impl_->connections_.end())
    {
        DAS_CORE_LOG_ERROR("Connection not found for remote_id = {}", remote_id);
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

DAS_CORE_IPC_NS_END
