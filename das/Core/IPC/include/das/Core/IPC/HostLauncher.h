/**
 * @file HostLauncher.h
 * @brief Host 进程启动器
 *
 * 负责启动 Host 进程、执行四次握手协议、管理生命周期。
 * 参考设计: IpcMultiProcessTestCommon.h 中的 ProcessLauncher 和 IpcClient
 */

#ifndef DAS_CORE_IPC_HOST_LAUNCHER_H
#define DAS_CORE_IPC_HOST_LAUNCHER_H

#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <memory>
#include <string>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

class IpcTransport;

/// 默认握手超时时间（毫秒）
constexpr uint32_t DEFAULT_HANDSHAKE_TIMEOUT_MS = 5000;

/**
 * @brief Host 进程启动器
 *
 * 负责启动 Host 进程并执行四次握手协议：
 * 1. Child -> Host: Hello (请求连接)
 * 2. Host -> Child: Welcome (分配 session_id)
 * 3. Child -> Host: Ready (确认就绪)
 * 4. Host -> Child: ReadyAck (确认就绪)
 */
class HostLauncher
{
public:
    HostLauncher();
    ~HostLauncher();

    // 禁止拷贝
    HostLauncher(const HostLauncher&) = delete;
    HostLauncher& operator=(const HostLauncher&) = delete;

    /**
     * @brief 启动 Host 进程并执行握手
     * @param host_exe_path Host 可执行文件路径
     * @param plugin_path 插件 manifest 路径（可选）
     * @param out_session_id 输出：分配的 session_id
     * @param timeout_ms 握手超时时间（毫秒），默认 5000ms
     * @return DasResult 成功返回 DAS_S_OK
     */
    DasResult Start(
        const std::string& host_exe_path,
        const std::string& plugin_path,
        uint16_t&          out_session_id,
        uint32_t           timeout_ms = DEFAULT_HANDSHAKE_TIMEOUT_MS);

    /**
     * @brief 停止 Host 进程
     *
     * 终止 Host 进程并清理 IPC 资源
     */
    void Stop();

    /**
     * @brief 检查进程是否正在运行
     * @return true 如果进程正在运行
     */
    [[nodiscard]]
    bool IsRunning() const;

    /**
     * @brief 获取进程 ID
     * @return 进程 ID，如果进程未运行返回 0
     */
    [[nodiscard]]
    uint32_t GetPid() const;

    /**
     * @brief 获取分配的 session_id
     * @return session_id，如果未握手成功返回 0
     */
    [[nodiscard]]
    uint16_t GetSessionId() const;

    /**
     * @brief 获取 IPC 传输接口
     * @return IPC 传输接口指针，如果未连接返回 nullptr
     */
    IpcTransport* GetTransport();

    /**
     * @brief 释放 IPC 传输接口的所有权
     * @return IPC 传输接口的所有权，调用者负责管理生命周期
     */
    std::unique_ptr<IpcTransport> ReleaseTransport();

private:
    /**
     * @brief 启动进程
     * @param exe_path 可执行文件路径
     * @param args 命令行参数
     * @return DasResult 成功返回 DAS_S_OK
     */
    DasResult LaunchProcess(
        const std::string&              exe_path,
        const std::vector<std::string>& args);

    /**
     * @brief 等待 Host 进程 IPC 资源就绪
     * @param timeout_ms 超时时间（毫秒）
     * @return DasResult 成功返回 DAS_S_OK
     */
    DasResult WaitForHostReady(uint32_t timeout_ms);

    /**
     * @brief 连接到 Host 进程的 IPC 资源
     * @return DasResult 成功返回 DAS_S_OK
     */
    DasResult ConnectToHost();

    /**
     * @brief 执行完整的四次握手协议
     * @param out_session_id 输出：分配的 session_id
     * @param timeout_ms 超时时间（毫秒）
     * @return DasResult 成功返回 DAS_S_OK
     */
    DasResult PerformFullHandshake(
        uint16_t& out_session_id,
        uint32_t  timeout_ms);

    /**
     * @brief 发送 Hello 消息
     * @param client_name 客户端名称
     * @return DasResult 成功返回 DAS_S_OK
     */
    DasResult SendHandshakeHello(const std::string& client_name);

    /**
     * @brief 接收 Welcome 消息
     * @param out_session_id 输出：分配的 session_id
     * @param timeout_ms 超时时间（毫秒）
     * @return DasResult 成功返回 DAS_S_OK
     */
    DasResult ReceiveHandshakeWelcome(
        uint16_t& out_session_id,
        uint32_t  timeout_ms);

    /**
     * @brief 发送 Ready 消息
     * @param session_id session_id
     * @return DasResult 成功返回 DAS_S_OK
     */
    DasResult SendHandshakeReady(uint16_t session_id);

    /**
     * @brief 接收 ReadyAck 消息
     * @param timeout_ms 超时时间（毫秒）
     * @return DasResult 成功返回 DAS_S_OK
     */
    DasResult ReceiveHandshakeReadyAck(uint32_t timeout_ms);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_HOST_LAUNCHER_H
