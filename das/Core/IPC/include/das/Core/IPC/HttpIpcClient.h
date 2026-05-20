#ifndef DAS_CORE_IPC_HTTP_IPC_CLIENT_H
#define DAS_CORE_IPC_HTTP_IPC_CLIENT_H

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcErrors.h>
#include <memory>
#include <string>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

// Forward declarations
class HttpIpcTransport;

/**
 * @brief HTTP/WebSocket IPC 客户端（RAII 风格）
 * @details 通过静态工厂方法 Connect() 创建已连接的客户端实例。
 *          构造函数为私有，确保所有实例都是完全初始化的。
 */
class HttpIpcClient
{
public:
    ~HttpIpcClient();

    HttpIpcClient(const HttpIpcClient&) = delete;
    HttpIpcClient& operator=(const HttpIpcClient&) = delete;

    /**
     * @brief 工厂方法：创建已连接的 HTTP IPC 客户端
     * @param io_context io_context 用于异步操作
     * @param host 服务器地址
     * @param port 服务器端口
     * @param my_pid 本进程 PID（用于握手）
     * @return 成功返回 unique_ptr<HttpIpcClient>，失败返回 DasResult 错误码
     *
     * RAII：返回的客户端已完成 TCP 连接 + WebSocket 升级 + IPC 握手，
     * 可直接使用 GetTransport() 发送/接收消息。
     */
    [[nodiscard]]
    static boost::asio::awaitable<
        std::variant<DasResult, std::unique_ptr<HttpIpcClient>>>
    Connect(
        boost::asio::io_context& io_context,
        const std::string&       host,
        const std::string&       port,
        uint32_t                 my_pid);

    /// 获取底层传输层（生命周期由 HttpIpcClient 管理）
    HttpIpcTransport* GetTransport() const;

    /// 释放传输层所有权（调用后 GetTransport() 返回 nullptr）
    std::unique_ptr<HttpIpcTransport> ReleaseTransport();

    /// 获取握手分配的 session_id
    uint16_t GetSessionId() const;

private:
    /**
     * @brief 私有构造函数，只接受已连接的 transport
     * @param transport 已完成连接和握手的传输层
     * @param session_id 握手分配的 session_id
     */
    explicit HttpIpcClient(
        std::unique_ptr<HttpIpcTransport> transport,
        uint16_t                          session_id);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_HTTP_IPC_CLIENT_H
