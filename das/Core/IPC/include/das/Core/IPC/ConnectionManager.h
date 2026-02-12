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
        class IpcTransport;
        class SharedMemoryPool;
        class IpcRunLoop;

        /**
         * @brief 连接资源信息
         *
         * 根据 B3.1 握手规范：
         * - Host 进程创建资源（消息队列、共享内存）
         * - Child 进程仅打开资源引用
         * - CleanupConnectionResources 清理时：
         *   - Host: 关闭并可能删除资源
         *   - Child: 仅释放引用，不删除资源
         */
        struct ConnectionInfo
        {
            uint16_t          host_id;
            uint16_t          plugin_id;
            bool              is_alive;
            uint64_t          last_heartbeat_ms;
            IpcTransport*     transport; ///< 消息队列传输（非拥有指针）
            SharedMemoryPool* shm_pool;  ///< 共享内存池（非拥有指针）
            IpcRunLoop* run_loop; ///< 运行循环（非拥有指针，含 pending_calls）
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
