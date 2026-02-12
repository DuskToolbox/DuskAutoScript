#ifndef DAS_HOST_HOST_CONFIG_H
#define DAS_HOST_HOST_CONFIG_H

#include <cstdint>
#include <das/IDasBase.h>
#include <string>

DAS_NS_BEGIN
namespace Host
{
    // B8 Host 进程配置常量

    // session_id 范围
    // 0x0000: 保留（无效值）
    // 0x0001-0xFFFE: 有效 session_id
    // 0xFFFF: 保留（最大值，避免溢出问题）
    constexpr uint16_t INVALID_SESSION_ID = 0;
    constexpr uint16_t MIN_SESSION_ID = 1;
    constexpr uint16_t MAX_SESSION_ID = 0xFFFE;

    // IPC 资源配置
    constexpr uint32_t DEFAULT_MAX_MESSAGES = 1024;
    constexpr uint32_t DEFAULT_MAX_MESSAGE_SIZE = 64 * 1024;          // 64KB
    constexpr size_t   DEFAULT_SHARED_MEMORY_SIZE = 16 * 1024 * 1024; // 16MB

    // 心跳配置
    constexpr uint32_t HEARTBEAT_INTERVAL_MS = 1000;
    constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 5000;

    // 连接超时
    constexpr uint32_t DEFAULT_CONNECTION_TIMEOUT_MS = 30000; // 30 seconds

    // 消息队列命名格式: DAS_Host_<host_pid>_MQ_H2P / DAS_Host_<host_pid>_MQ_P2H
    // 共享内存命名格式: DAS_Host_<host_pid>_SHM

    /**
     * @brief 生成消息队列名称
     * @param host_pid Host 进程 PID
     * @param is_host_to_plugin true 表示 Host->Plugin 方向
     */
    inline std::string MakeMessageQueueName(
        uint32_t host_pid,
        bool     is_host_to_plugin)
    {
        return std::string("DAS_Host_") + std::to_string(host_pid) + "_MQ_"
               + (is_host_to_plugin ? "H2P" : "P2H");
    }

    /**
     * @brief 生成共享内存名称
     * @param host_pid Host 进程 PID
     */
    inline std::string MakeSharedMemoryName(uint32_t host_pid)
    {
        return std::string("DAS_Host_") + std::to_string(host_pid) + "_SHM";
    }
}
DAS_NS_END

#endif // DAS_HOST_HOST_CONFIG_H
