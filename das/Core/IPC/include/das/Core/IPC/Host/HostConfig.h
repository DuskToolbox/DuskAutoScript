#ifndef DAS_CORE_IPC_HOST_HOST_CONFIG_H
#define DAS_CORE_IPC_HOST_HOST_CONFIG_H

#include <cstdint>
#include <das/IDasBase.h>
#include <string>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
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
            constexpr uint32_t DEFAULT_MAX_MESSAGE_SIZE = 64 * 1024; // 64KB
            constexpr size_t   DEFAULT_SHARED_MEMORY_SIZE =
                16 * 1024 * 1024; // 16MB

            // 心跳配置
            constexpr uint32_t HEARTBEAT_INTERVAL_MS = 1000;
            constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 5000;

            // 连接超时
            constexpr uint32_t DEFAULT_CONNECTION_TIMEOUT_MS =
                30000; // 30 seconds

            // 新命名格式: das_ipc_<main_pid>_<host_pid>_m2h/h2m
            // 旧命名格式: DAS_Host_<host_pid>_MQ_M2H/H2M (兼容保留)

            /**
             * @brief 生成消息队列名称（新格式）
             * @param main_pid 主进程 PID
             * @param host_pid Host 进程 PID
             * @param is_main_to_host true 表示 Main->Host 方向
             */
            inline std::string MakeMessageQueueName(
                uint32_t main_pid,
                uint32_t host_pid,
                bool     is_main_to_host)
            {
                return std::string("das_ipc_") + std::to_string(main_pid)
                       + "_" + std::to_string(host_pid) + "_"
                       + (is_main_to_host ? "m2h" : "h2m");
            }

            /**
             * @brief 生成共享内存名称（新格式）
             * @param main_pid 主进程 PID
             * @param host_pid Host 进程 PID
             */
            inline std::string MakeSharedMemoryName(
                uint32_t main_pid,
                uint32_t host_pid)
            {
                return std::string("das_ipc_") + std::to_string(main_pid)
                       + "_" + std::to_string(host_pid) + "_shm";
            }

            /**
             * @brief 生成消息队列名称（旧格式，兼容用）
             * @param host_pid Host 进程 PID
             * @param is_main_to_host true 表示 Main->Host 方向
             */
            inline std::string MakeLegacyMessageQueueName(
                uint32_t host_pid,
                bool     is_main_to_host)
            {
                return std::string("DAS_Host_") + std::to_string(host_pid)
                       + "_MQ_" + (is_main_to_host ? "M2H" : "H2M");
            }

            /**
             * @brief 生成共享内存名称（旧格式，兼容用）
             * @param host_pid Host 进程 PID
             */
            inline std::string MakeLegacySharedMemoryName(uint32_t host_pid)
            {
                return std::string("DAS_Host_") + std::to_string(host_pid)
                       + "_SHM";
            }
        } // namespace Host
    } // namespace IPC
} // namespace Core
DAS_NS_END

#endif // DAS_CORE_IPC_HOST_HOST_CONFIG_H
