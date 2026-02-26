#include <atomic>
#include <chrono>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/IpcTransport.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <shared_mutex>
#include <unordered_map>

DAS_CORE_IPC_NS_BEGIN
struct ConnectionManager::Impl
{
    std::unordered_map<uint16_t, ConnectionInfo> connections_;
    mutable std::shared_mutex                    connections_mutex_;
    std::atomic<bool>                            running_{false};
    std::thread                                  heartbeat_thread_;
    uint16_t                                     local_id_{0};
};

ConnectionManager::ConnectionManager() : impl_(std::make_unique<Impl>()) {}

ConnectionManager::~ConnectionManager()
{
    StopHeartbeatThread();
    Shutdown();
    StopHeartbeatThread();
    Shutdown();
}

ConnectionManager& ConnectionManager::GetInstance()
{
    static ConnectionManager instance;
    return instance;
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

    return DAS_S_OK;
}

DasResult ConnectionManager::RegisterConnection(
    uint16_t remote_id,
    uint16_t local_id)
{
    ConnectionInfo info{
        .host_id = remote_id,
        .plugin_id = local_id,
        .is_alive = true,
        .last_heartbeat_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count()),
        .transport = nullptr,
        .shm_pool = nullptr,
        .run_loop = nullptr};

    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);
    impl_->connections_[remote_id] = info;

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
        return false;
    }
    return true;
}

DasResult ConnectionManager::GetConnection(
    uint16_t        session_id,
    ConnectionInfo& out_info) const
{
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    auto it = impl_->connections_.find(session_id);
    if (it == impl_->connections_.end())
    {
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    out_info = it->second;
    return DAS_S_OK;
}

IpcTransport* ConnectionManager::GetTransport(uint16_t session_id) const
{
    std::shared_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    auto it = impl_->connections_.find(session_id);
    if (it == impl_->connections_.end())
    {
        return nullptr;
    }

    return it->second.transport;
}

DasResult ConnectionManager::RegisterHostTransport(
    uint16_t          session_id,
    IpcTransport*     transport,
    SharedMemoryPool* shm_pool,
    IpcRunLoop*       run_loop)
{
    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    // 如果连接已存在，更新 transport
    auto it = impl_->connections_.find(session_id);
    if (it != impl_->connections_.end())
    {
        it->second.transport = transport;
        it->second.shm_pool = shm_pool;
        it->second.run_loop = run_loop;
        it->second.is_alive = true;
        return DAS_S_OK;
    }

    // 创建新连接条目
    ConnectionInfo info{
        .host_id = session_id,
        .plugin_id = 0,
        .is_alive = true,
        .last_heartbeat_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count()),
        .transport = transport,
        .shm_pool = shm_pool,
        .run_loop = run_loop};

    impl_->connections_[session_id] = info;
    return DAS_S_OK;
}

DasResult ConnectionManager::SetConnectionAlive(
    uint16_t session_id,
    bool     is_alive)
{
    std::unique_lock<std::shared_mutex> lock(impl_->connections_mutex_);

    auto it = impl_->connections_.find(session_id);
    if (it == impl_->connections_.end())
    {
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
                        CleanupConnectionResources(it->first, impl_->local_id_);
                        it = impl_->connections_.erase(it);
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
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    ConnectionInfo& info = it->second;

    // 清理顺序：pending_calls -> 共享内存 -> 消息队列 -> 连接状态
    // 注意：B3.1 规范 - Child 仅释放引用，不删除资源（Host 负责删除）

    if (info.run_loop != nullptr && info.run_loop->IsRunning())
    {
        info.run_loop->Stop();
    }

    info.shm_pool = nullptr;
    info.transport = nullptr;
    info.run_loop = nullptr;
    info.is_alive = false;

    return DAS_S_OK;
}
DAS_CORE_IPC_NS_END
