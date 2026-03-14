#ifndef DAS_CORE_IPC_CONNECTION_MANAGER_H
#define DAS_CORE_IPC_CONNECTION_MANAGER_H

#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <cstdint>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/IDasBase.h>
#include <memory>
#include <string>
#include <thread>

#include <das/Core/IPC/Config.h>
#include <das/DasConfig.h>

DAS_CORE_IPC_NS_BEGIN

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
 *
 * ConnectionManager 持有 IHostLauncher 引用（DasPtr），
 * Transport 由 HostLauncher 永久持有。
 */
struct ConnectionInfo
{
    uint16_t              host_id;
    uint16_t              plugin_id;
    bool                  is_alive;
    uint64_t              last_heartbeat_ms;
    DasPtr<IHostLauncher> launcher; ///< HostLauncher 实例（DasPtr 持有引用）
    SharedMemoryPool* DAS_LIFETIMEBOUND shm_pool =
        nullptr; ///< 共享内存池（非拥有指针）
};

/**
 * @brief 连接管理器（非单例模式）
 *
 * 由 IpcRunLoop 持有，负责管理 HostLauncher 实例。
 * ConnectionManager 持有 DasPtr<IHostLauncher>，Transport 由 HostLauncher
 * 拥有。
 *
 * 心跳超时清理：释放 DasPtr -> 引用计数归零 -> HostLauncher 析构 -> 自动清理
 * Host 进程
 *
 * RAII 模式：使用 Create() 工厂函数创建，析构自动清理
 */
class ConnectionManager
{
public:
    /**
     * @brief 工厂函数：创建 ConnectionManager 实例
     * @param local_id 本地 ID
     * @return std::unique_ptr<ConnectionManager> ConnectionManager 智能指针
     */
    static std::unique_ptr<ConnectionManager> Create(uint16_t local_id);

    ~ConnectionManager();

    DasResult Shutdown();

    // 禁止拷贝
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    DasResult RegisterConnection(uint16_t remote_id, uint16_t local_id);
    DasResult UnregisterConnection(uint16_t remote_id, uint16_t local_id);

    DasResult SendHeartbeat(uint16_t remote_id);

    bool IsConnectionAlive(uint16_t remote_id) const;

    /**
     * @brief 获取所有已连接的 session_id 列表
     *
     * @return std::vector<uint16_t> session_id 列表
     */
    std::vector<uint16_t> GetConnectedSessions() const;

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
     * @brief 注册 HostLauncher（转移所有权）
     *
     * ConnectionManager 获取 DasPtr 所有权， Transport 由 HostLauncher 拥有。
     *
     * @param session_id 会话 ID
     * @param launcher HostLauncher 实例（DasPtr 移动）
     * @return DasResult DAS_S_OK 成功
     */
    DasResult RegisterHostLauncher(
        uint16_t              session_id,
        DasPtr<IHostLauncher> launcher);

    /**
     * @brief 取消注册 HostLauncher
     *
     * @param session_id 目标会话ID
     * @return DasResult DAS_S_OK 成功，DAS_E_IPC_OBJECT_NOT_FOUND 未找到
     */
    DasResult UnregisterHostLauncher(uint16_t session_id);

    /**
     * @brief 获取 IHostLauncher
     *
     * @param session_id 目标会话ID
     * @return DasPtr<IHostLauncher> HostLauncher 指针，不存在返回 nullptr
     */
    DasPtr<IHostLauncher> GetLauncher(uint16_t session_id) const;

    /**
     * @brief 获取连接的传输层（委托给 HostLauncher::GetTransport()）
     *
     * 生命周期标记：返回的指针在 ConnectionManager 析构后失效
     *
     * @param session_id 目标会话ID
     * @return DefaultAsyncIpcTransport* 传输层指针（不持有所有权），不存在返回
     * nullptr
     */
    DefaultAsyncIpcTransport* DAS_LIFETIMEBOUND
    GetTransport(uint16_t session_id) const;

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

    /**
     * @brief 向所有已连接的 Host 发送 HEARTBEAT 消息
     *
     * 异步发送，不等待回复。回复会在消息循环中处理。
     *
     * @return DasResult DAS_S_OK 成功
     */
    DasResult SendHeartbeatToAll();

    /**
     * @brief 更新指定连接的心跳时间戳
     *
     * 当收到 HEARTBEAT RESPONSE 时调用。
     *
     * @param session_id 会话 ID
     */
    void UpdateHeartbeatTimestamp(uint16_t session_id);

    /**
     * @brief 转发消息到目标 session
     *
     * 将消息转发到指定的 target_session_id。
     * 转发时会修改 header.source_session_id 为本地 session_id（用于响应路由）。
     *
     * @param target_session_id 目标 session ID
     * @param header 消息头（会被修改 source_session_id）
     * @param body 消息体
     * @param body_size 消息体大小
     * @return DasResult DAS_S_OK 成功, DAS_E_IPC_OBJECT_NOT_FOUND 目标不存在
     */
    boost::asio::awaitable<DasResult> ForwardMessage(
        uint16_t              target_session_id,
        const ValidatedIPCMessageHeader& header,
        const uint8_t*        body,
        size_t                body_size);

    static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 1000;
    static constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 5000;

private:
    // 私有构造函数 - 只能通过 Create() 工厂函数调用
    ConnectionManager();
    friend class std::unique_ptr<ConnectionManager>;

    // 私有初始化函数 - 只能由 Create() 工厂函数调用
    DasResult Initialize(uint16_t local_id);

    // 私有清理函数 - 只能由析构函数调用
    void Uninitialize();

    DasResult CleanupConnectionResources(uint16_t remote_id, uint16_t local_id);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_CONNECTION_MANAGER_H
