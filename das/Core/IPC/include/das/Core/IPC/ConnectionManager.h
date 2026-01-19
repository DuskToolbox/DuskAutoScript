#ifndef DAS_CORE_IPC_CONNECTION_MANAGER_H
#define DAS_CORE_IPC_CONNECTION_MANAGER_H

#include <atomic>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/IDasBase.h>
#include <memory>
#include <string>
#include <thread>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        struct ConnectionInfo
        {
            uint16_t host_id;
            uint16_t plugin_id;
            bool     is_alive;
            uint64_t last_heartbeat_ms;
        };

        class ConnectionManager
        {
        public:
            ConnectionManager();
            ~ConnectionManager();

            DasResult Initialize(uint16_t local_id);
            DasResult Shutdown();

            DasResult RegisterConnection(uint16_t remote_id, uint16_t local_id);
            DasResult UnregisterConnection(
                uint16_t remote_id,
                uint16_t local_id);

            DasResult SendHeartbeat(uint16_t remote_id);

            bool IsConnectionAlive(uint16_t remote_id) const;

            void StartHeartbeatThread();
            void StopHeartbeatThread();

            static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 1000;
            static constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 5000;

        private:
            DasResult CleanupConnectionResources(
                uint16_t remote_id,
                uint16_t local_id);

            struct Impl;
            std::unique_ptr<Impl> impl_;
        };
    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_CONNECTION_MANAGER_H
