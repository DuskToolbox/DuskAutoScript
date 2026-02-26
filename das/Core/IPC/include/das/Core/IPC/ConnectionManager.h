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

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN
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
    IpcRunLoop*       run_loop;  ///< 运行循环（非拥有指针，含 pending_calls）
};

class ConnectionManager
{
public:
    ConnectionManager();
    ~ConnectionManager();

    static ConnectionManager& GetInstance();

    DasResult Initialize(uint16_t local_id);
    DasResult Shutdown();

    DasResult RegisterConnection(uint16_t remote_id, uint16_t local_id);
    DasResult UnregisterConnection(uint16_t remote_id, uint16_t local_id);

    DasResult SendHeartbeat(uint16_t remote_id);

    bool IsConnectionAlive(uint16_t remote_id) const;

    /**
     * @brief 获取到指定 session 的连接信息
     *
     * @param session_id 目标会话ID
     * @param out_info 输出连接信息
     * @return DasResult DAS_S_OK 成功，DAS_E_IPC_OBJECT_NOT_FOUND 未找到
     */
    DasResult GetConnection(uint16_t session_id, ConnectionInfo& out_info)
        const;

    /**
     * @brief 获取连接的传输层（用于发送消息）
     *
     * @param session_id 目标会话ID
     * @return IpcTransport* 传输层指针（不持有所有权），不存在返回 nullptr
     */
    IpcTransport* GetTransport(uint16_t session_id) const;

    /**
     * @brief 注册 Host 进程的传输层
     *
     * 用于主进程转发消息到目标 Host
     *
     * @param session_id 目标会话ID
     * @param transport 传输层指针（不持有所有权）
     * @param shm_pool 共享内存池指针（可选，不持有所有权）
     * @param run_loop 运行循环指针（可选，不持有所有权）
     * @return DasResult DAS_S_OK 成功
     */
    DasResult RegisterHostTransport(
        uint16_t          session_id,
        IpcTransport*     transport,
        SharedMemoryPool* shm_pool = nullptr,
        IpcRunLoop*       run_loop = nullptr);

    /**
     * @brief 更新连接的活跃状态
     *
     * @param session_id 目标会话ID
     * @param is_alive 是否活跃
     * @return DasResult DAS_S_OK 成功
     */
    DasResult SetConnectionAlive(uint16_t session_id, bool is_alive);

    void StartHeartbeatThread();
    void StopHeartbeatThread();

    static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 1000;
    static constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 5000;

private:
    DasResult CleanupConnectionResources(uint16_t remote_id, uint16_t local_id);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_CONNECTION_MANAGER_H
