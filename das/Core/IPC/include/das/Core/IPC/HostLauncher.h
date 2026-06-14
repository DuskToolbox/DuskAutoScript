/**
 * @file HostLauncher.h
 * @brief Host 进程启动器
 *
 * 负责启动 Host 进程、执行四次握手协议、管理生命周期。
 * 参考设计: IpcMultiProcessTestCommon.h 中的 ProcessLauncher 和 IpcClient
 */

#ifndef DAS_CORE_IPC_HOST_LAUNCHER_H
#define DAS_CORE_IPC_HOST_LAUNCHER_H

#include <atomic>
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IHostConnection.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>

// Forward declaration for io_context
namespace boost::asio
{
    class io_context;
}

DAS_CORE_IPC_NS_BEGIN

// 前向声明主模板
template <typename Signature>
class GuardedCallback;

/**
 * @brief 跨线程 callback 封装（private std::mutex + std::function）
 *
 * Phase 80.2 锁定方案 D-01/D-03/D-05/D-06 的载体：把 HostLauncher 的
 * `on_process_exit_` / `on_heartbeat_timeout_` 裸 std::function
 * 成员换成此封装， 让 CR-01（std::function 跨线程读写 UB）与
 * CR-02（ClearCallbacks 持锁 = drain 在途回调的轻量等价）由一个类型统一承担。
 *
 * INV-01（回调内不得重入本 slot）：Invoke 持锁执行 callback_。非递归 std::mutex
 *   保证回调内部若回头调用同一 slot 的 Set/Clear/Invoke 立即自死锁暴露 ——
 *   recursive_mutex 救不了 Clear() 置空正在执行的 std::function（= 析构正在
 *   执行的 this->callback_ = UB），故非递归锁比递归锁更安全。
 *   当前 OnHeartbeatTimeout / OnHostProcessExit -> CleanupPluginByGuid 只 erase
 *   索引，不触碰回调 slot，满足此约束。
 */
template <typename Ret, typename... Args>
class GuardedCallback<Ret(Args...)>
{
public:
    using Callback = std::function<Ret(Args...)>;

    GuardedCallback() = default;
    ~GuardedCallback() = default;

    GuardedCallback(const GuardedCallback&) = delete;
    GuardedCallback& operator=(const GuardedCallback&) = delete;
    GuardedCallback(GuardedCallback&&) = delete;
    GuardedCallback& operator=(GuardedCallback&&) = delete;

    /// 持锁替换 callback_。调用方通常是 PluginManager / IPC setup 线程。
    void Set(Callback cb)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(cb);
    }

    /// 持锁置空。析构 / Shutdown / Stop 路径调用。
    /// 关键：持锁 = Invoke 若在执行则等待其完成（因为 Invoke 持同一把锁），
    /// 这是 CR-02 "drain 轻量等价" 的实现机制。
    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = nullptr;
    }

    /**
     * @brief 持锁执行 callback_（D-01 持锁调用，非取副本锁外调）
     *
     * INV-01: callback must NOT re-enter Set/Clear/Invoke of this slot.
     *   - 非递归 std::mutex：重入立即死锁（同线程二次 lock）
     *   - Clear() during in-flight 会析构正在执行的 std::function = UB
     *   - 两者皆靠"持锁执行 + 非递归锁"让违反立即暴露
     *
     * 故意的"最小持锁原则"违反：取副本锁外调（写法 B）会让 ClearCallbacks
     * 只等到"取副本完"，等不到"回调执行完"，CR-02 的 in-flight UAF 回来。
     * 持锁执行是 ClearCallbacks 能 drain 在途回调的必要形态。
     */
    void Invoke(Args... args)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (callback_)
        {
            callback_(args...);
        }
    }

private:
    std::mutex mutex_;
    Callback   callback_;
};

class HostLauncher final : public IHostLauncher, public IHostConnection
{
public:
    using OnRegisterCallback = std::function<DasResult()>;

    /**
     * @brief Host 进程退出回调
     * @param session_id 退出进程的 session_id
     * @param exit_code 进程退出码（如果正常退出）
     */
    using OnProcessExitCallback =
        std::function<void(uint16_t session_id, int exit_code)>;

    /**
     * @brief 心跳超时断连回调
     * @param guid 关联的插件 GUID
     */
    using OnHeartbeatTimeoutCallback = std::function<void(DasGuid guid)>;

    explicit HostLauncher(
        boost::asio::io_context& io_ctx,
        uint16_t                 session_id,
        OnRegisterCallback       on_register = nullptr);
    ~HostLauncher() override;

    void ClearCallbacks() override
    {
        // on_register_ 是单线程 Setup 路径成员，不在并发 scope，保留裸置空。
        // on_process_exit_ / on_heartbeat_timeout_ 走 GuardedCallback 持锁
        // Clear， 持锁语义 = drain 在途回调（CR-02 屏障）。
        on_register_ = nullptr;
        on_process_exit_slot_.Clear();
        on_heartbeat_timeout_slot_.Clear();
    }

    /**
     * @brief 设置 Host 进程退出回调
     * @param callback 进程退出时触发的回调（在 io_context 线程上执行）
     */
    void SetOnProcessExit(OnProcessExitCallback callback);

    /**
     * @brief 设置心跳超时断连回调
     * @param callback 心跳超时时触发的回调
     */
    void SetOnHeartbeatTimeout(OnHeartbeatTimeoutCallback callback);

    /**
     * @brief 设置关联的插件 GUID
     * @param guid 插件 GUID
     */
    void SetAssociatedGuid(DasGuid guid);

    /**
     * @brief 获取关联的插件 GUID
     * @return DasGuid 关联的插件 GUID
     */
    [[nodiscard]]
    DasGuid GetAssociatedGuid() const;

    /**
     * @brief 触发心跳超时回调
     *
     * 由 ConnectionManager 在心跳超时时调用。
     * 内部调用 on_heartbeat_timeout_ 回调（传递 associated_guid_）。
     * 回调在调用线程上同步执行。
     */
    void NotifyHeartbeatTimeout() override;

    /**
     * @brief 终止 Host 进程（不发送 GOODBYE）
     *
     * 用于心跳超时场景，直接 terminate 进程而不发 GOODBYE 消息。
     * 与 Stop() 不同，此方法不会尝试优雅关闭。
     * 进程句柄被释放以避免阻塞。
     */
    void TerminateIfRunning() override;

    HostLauncher(const HostLauncher&) = delete;
    HostLauncher& operator=(const HostLauncher&) = delete;

    DasResult StartAsync(
        const std::string&            host_exe_path,
        IDasAsyncHandshakeOperation** pp_out_operation) override;

    DasResult Start(
        const std::string& host_exe_path,
        uint16_t&          out_session_id,
        uint32_t           timeout_ms) override;

    DasResult StartWithDesc(
        const HostLaunchDesc* p_desc,
        uint32_t              timeout_ms,
        uint16_t*             p_out_session_id) override;

    void Stop() override;

    [[nodiscard]]
    bool IsRunning() const override;

    [[nodiscard]]
    uint32_t GetPid() const override;

    [[nodiscard]]
    uint16_t GetSessionId() const override;

    boost::asio::io_context& GetIoContext() DAS_LIFETIMEBOUND override;

    TransportLookupResult GetTransport() override;

    uint32_t  AddRef() override;
    uint32_t  Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp) override;

private:
    DasResult LaunchProcess(
        const std::string&                exe_path,
        const std::vector<std::string>&   args,
        const std::optional<std::string>& working_directory = std::nullopt,
        const std::optional<std::vector<std::string>>& environment =
            std::nullopt);

    DasResult StartLaunchSequence(
        const std::string&                             exe_path,
        const std::vector<std::string>&                args,
        const std::optional<std::string>&              working_directory,
        const std::optional<std::vector<std::string>>& environment,
        uint16_t&                                      out_session_id,
        uint32_t                                       timeout_ms);

    DasResult WaitForHostReady(uint32_t timeout_ms);
    DasResult ConnectToHost(uint32_t timeout_ms);
    DasResult PerformFullHandshake(
        uint16_t& out_session_id,
        uint32_t  timeout_ms);

    DasResult SendHandshakeHello(const std::string& client_name);
    DasResult ReceiveHandshakeWelcome(
        uint16_t& out_session_id,
        uint32_t  timeout_ms);
    DasResult SendHandshakeReady(uint16_t session_id);
    DasResult ReceiveHandshakeReadyAck(uint32_t timeout_ms);

    // === 协程版本的握手方法 ===

    /**
     * @brief 完整握手流程（协程版本）
     */
    boost::asio::awaitable<DasResult> PerformFullHandshakeAsync(
        uint16_t& out_session_id,
        uint32_t  timeout_ms);

    /**
     * @brief 发送 Hello 消息（协程版本）
     */
    boost::asio::awaitable<DasResult> SendHandshakeHelloAsync(
        const std::string& client_name);

    /**
     * @brief 接收 Welcome 消息（协程版本）
     */
    boost::asio::awaitable<DasResult> ReceiveHandshakeWelcomeAsync(
        uint16_t& out_session_id,
        uint32_t  timeout_ms);

    /**
     * @brief 发送 Ready 消息（协程版本）
     */
    boost::asio::awaitable<DasResult> SendHandshakeReadyAsync(
        uint16_t session_id);

    /**
     * @brief 接收 ReadyAck 消息（协程版本）
     */
    boost::asio::awaitable<DasResult> ReceiveHandshakeReadyAckAsync(
        uint32_t timeout_ms);

    // === 异步启动相关方法 ===

    /**
     * @brief 异步等待 Host IPC 资源就绪（使用 steady_timer 替代 Sleep）
     */
    boost::asio::awaitable<DasResult> WaitForHostReadyAsync(
        uint32_t timeout_ms);

    /**
     * @brief 异步连接到 Host 进程
     */
    boost::asio::awaitable<DasResult> ConnectToHostAsync();

    /**
     * @brief 异步启动完整流程协程（5 阶段）
     *
     * op 参数实际类型为 HandshakeAsyncOperationImpl*（在 .cpp 中定义），
     * 通过 IDasAsyncHandshakeOperation* 传递以避免头文件暴露实现类。
     */
    boost::asio::awaitable<void> RunAsync(
        IDasAsyncHandshakeOperation*       op,
        std::shared_ptr<std::atomic<bool>> alive,
        std::string                        exe_path,
        uint32_t                           timeout_ms);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    uint16_t           session_id_ = 0;
    OnRegisterCallback on_register_;
    // INV-04: HostLauncher 被 host_launchers_（PluginManager，按 GUID）+
    //         ConnectionManager::hosts_（按 session_id）双重 DasPtr 持有。
    //         erase 一份后计数仍 ≥1，HostLauncher 不析构，callback_mutex_
    //         不析构。这是 ClearCallbacks 持锁 drain 在途回调成立的隐蔽依赖，
    //         明文化后才有维护保障。
    GuardedCallback<void(uint16_t, int)> on_process_exit_slot_;
    GuardedCallback<void(DasGuid)>       on_heartbeat_timeout_slot_;
    DasGuid                              associated_guid_{};
    /// 保护 associated_guid_ 的并发读写（NotifyHeartbeatTimeout 读、
    /// SetAssociatedGuid 写）。DasGuid 是 POD 无法 atomic，故用独立 mutex；
    /// 维持单一锁职责，不让 slot 锁内嵌套 guid 锁。
    mutable std::mutex associated_guid_mutex_;
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_HOST_LAUNCHER_H
