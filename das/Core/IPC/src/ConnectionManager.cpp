#include <atomic>
#include <chrono>
#include <das/Core/IPC/ConnectionManager.h>
#include <unordered_map>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        struct ConnectionManager::Impl
        {
            std::unordered_map<uint16_t, ConnectionInfo> connections_;
            mutable std::shared_mutex                    connections_mutex_;
            std::atomic<bool>                            running_{false};
            std::thread                                  heartbeat_thread_;
            uint16_t                                     local_id_{0};
        };

        ConnectionManager::ConnectionManager() : impl_(std::make_unique<Impl>())
        {
        }

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
                .last_heartbeat_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count()};

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

            return it->second.is_alive;
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
                            std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                std::chrono::steady_clock::now()
                                    .time_since_epoch())
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
                                CleanupConnectionResources(
                                    it->first,
                                    impl_->local_id_);
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
            // TODO: Clean up message queues and shared memory
            // This will be implemented when MessageQueueTransport and
            // SharedMemoryPool are ready

            return DAS_S_OK;
        }
    }
}
DAS_NS_END
