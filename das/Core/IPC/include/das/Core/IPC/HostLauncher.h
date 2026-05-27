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
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IInternalHost.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>

// Forward declaration for io_context
namespace boost::asio
{
    class io_context;
}

DAS_CORE_IPC_NS_BEGIN

class HostLauncher final : public IHostLauncher, public IInternalHost
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
        on_register_ = nullptr;
        on_process_exit_ = nullptr;
        on_heartbeat_timeout_ = nullptr;
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

    uint16_t                   session_id_ = 0;
    OnRegisterCallback         on_register_;
    OnProcessExitCallback      on_process_exit_;
    OnHeartbeatTimeoutCallback on_heartbeat_timeout_;
    DasGuid                    associated_guid_{};
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_HOST_LAUNCHER_H
