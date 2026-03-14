#ifndef DAS_CORE_IPC_IPC_TRANSPORT_H
#define DAS_CORE_IPC_IPC_TRANSPORT_H

#include <boost/interprocess/ipc/message_queue.hpp>
#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/DasExport.h>
#include <das/IDasBase.h>
#include <memory>
#include <string>
#include <vector>

#include <das/Core/IPC/Config.h>
#include <das/DasConfig.h>
#include <das/Utils/Expected.h>

DAS_CORE_IPC_NS_BEGIN
class SharedMemoryPool;

/// @brief 接收结果 - 包含验证后的 Header 和 Body
struct ReceiveResult
{
    ReceiveResult() = default;

    std::optional<ValidatedIPCMessageHeader> header;
    std::vector<uint8_t>                     body;
};

class IpcTransport
{
    // 允许 std::unique_ptr 访问私有构造函数
    friend class std::unique_ptr<IpcTransport>;

public:
    /**
     * @brief 创建 IpcTransport 实例（服务端模式）
     * @param host_queue_name 主机队列名称
     * @param plugin_queue_name 插件队列名称
     * @param max_message_size 最大消息大小
     * @param max_messages 最大消息数量
     * @return IpcTransport 实例的智能指针
     */
    static DAS::Utils::Expected<std::unique_ptr<IpcTransport>> Create(
        const std::string& host_queue_name,
        const std::string& plugin_queue_name,
        uint32_t           max_message_size,
        uint32_t           max_messages);

    /**
     * @brief 连接到现有队列（客户端模式）
     * @param host_queue_name 主机队列名称
     * @param plugin_queue_name 插件队列名称
     * @return IpcTransport 实例的智能指针
     */
    static DAS::Utils::Expected<std::unique_ptr<IpcTransport>> Connect(
        const std::string& host_queue_name,
        const std::string& plugin_queue_name);

    ~IpcTransport();

    DasResult Send(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

    /// @brief 接收消息并返回验证后的 Header
    /// @param timeout_ms 超时时间（毫秒），0 表示无限等待
    /// @return 接收成功返回 ReceiveResult，失败或超时返回 std::nullopt
    [[nodiscard]]
    std::optional<ReceiveResult> Receive(uint32_t timeout_ms);

    DasResult SetSharedMemoryPool(SharedMemoryPool* pool);

    [[nodiscard]]
    bool IsConnected() const;

    static std::string MakeQueueName(
        uint32_t main_pid,
        uint32_t host_pid,
        bool     is_main_to_host);

private:
    IpcTransport();
    IpcTransport(
        const std::string& host_queue_name,
        const std::string& plugin_queue_name,
        uint32_t           max_message_size,
        uint32_t           max_messages,
        bool               is_server);

    void Uninitialize();

    DasResult SendSmallMessage(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

    DasResult SendLargeMessage(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_TRANSPORT_H
