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
#include <memory>
#include <string>

#include <boost/asio/awaitable.hpp>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>

// Forward declaration for io_context
namespace boost::asio
{
    class io_context;
}

DAS_CORE_IPC_NS_BEGIN

// Forward declaration for MainProcess::IpcContext (for auto-registration)
namespace MainProcess
{
    class IpcContext;
}

class HostLauncher final : public IHostLauncher
{
public:
    explicit HostLauncher(boost::asio::io_context& io_ctx);
    ~HostLauncher() override;

    void SetIpcContext(MainProcess::IIpcContext* ctx) { ipc_context_ = ctx; }

    HostLauncher(const HostLauncher&) = delete;
    HostLauncher& operator=(const HostLauncher&) = delete;

    DasResult StartAsync(
        const std::string&            host_exe_path,
        IDasAsyncHandshakeOperation** pp_out_operation) override;

    DasResult Start(
        const std::string& host_exe_path,
        uint16_t&          out_session_id,
        uint32_t           timeout_ms) override;

    void Stop() override;

    [[nodiscard]]
    bool IsRunning() const override;

    [[nodiscard]]
    uint32_t GetPid() const override;

    [[nodiscard]]
    uint16_t GetSessionId() const override;

    DefaultAsyncIpcTransport* GetTransport();

    uint32_t  AddRef() override;
    uint32_t  Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp) override;

private:
    DasResult LaunchProcess(
        const std::string&              exe_path,
        const std::vector<std::string>& args);

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

    MainProcess::IIpcContext* ipc_context_ = nullptr;
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_HOST_LAUNCHER_H
