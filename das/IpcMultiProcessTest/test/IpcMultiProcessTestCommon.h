/**
 * @file IpcMultiProcessTestCommon.h
 * @brief IPC 多进程测试共享组件
 *
 * 包含用于 IPC 多进程测试的测试夹具。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/Host/HostConfig.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/IpcTransport.h>
#include <das/Core/IPC/MainProcess/MainProcessServer.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

#include <boost/asio/io_context.hpp>
#include <boost/process/v2/pid.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/system/error_code.hpp>
#include <das/Core/IPC/HandshakeSerialization.h>

// ============================================================
// 全局测试配置 - 环境变量与超时设置
// ============================================================
//
// 环境变量：
//   DAS_DEBUG=1          - 调试模式，所有超时变为 0（无限等待）
//   DAS_HOST_EXE_PATH    - DasHost.exe 的完整路径
//   DAS_PLUGIN_DIR       - 插件目录路径
//
// 示例（Windows）：
//   set DAS_DEBUG=1 && set DAS_HOST_EXE_PATH=C:\path\DasHost.exe && set
//   DAS_PLUGIN_DIR=C:\path\plugins && IpcMultiProcessTest.exe
//
// 示例（Linux/macOS）：
//   DAS_DEBUG=1 DAS_HOST_EXE_PATH=/path/DasHost DAS_PLUGIN_DIR=/path/plugins
//   ./IpcMultiProcessTest
//

namespace IpcTestConfig
{
    namespace detail
    {
        inline bool IsDebugMode()
        {
            const char* debug = std::getenv("DAS_DEBUG");
            return debug != nullptr
                   && (std::string(debug) == "1"
                       || std::string(debug) == "true");
        }
    } // namespace detail

    /**
     * @brief 获取启动 Host 进程的超时时间（毫秒）
     *
     * 当设置 DAS_DEBUG=1 环境变量时，返回 0（无限等待）。
     * 否则返回默认的 10000 毫秒（10秒）。
     */
    inline uint32_t GetHostStartTimeoutMs()
    {
        return detail::IsDebugMode() ? 0 : 10000;
    }

    /**
     * @brief 获取加载插件的超时时间
     *
     * 当设置 DAS_DEBUG=1 环境变量时，返回 0（无限等待）。
     * 否则返回默认的 30000 毫秒（30秒）。
     */
    inline std::chrono::milliseconds GetPluginLoadTimeout()
    {
        return std::chrono::milliseconds(detail::IsDebugMode() ? 0 : 30000);
    }

    /**
     * @brief 检查是否为调试模式
     */
    inline bool IsDebugMode() { return detail::IsDebugMode(); }

    /**
     * @brief 获取 DasHost 可执行文件路径
     *
     * 从环境变量 DAS_HOST_EXE_PATH 读取。
     * @return DasHost.exe 的完整路径
     * @throws std::runtime_error 如果环境变量未设置或文件不存在
     */
    inline std::string GetDasHostPath()
    {
        const char* path = std::getenv("DAS_HOST_EXE_PATH");
        if (path == nullptr || strlen(path) == 0)
        {
            throw std::runtime_error(
                "DAS_HOST_EXE_PATH environment variable is not set");
        }
        if (!std::filesystem::exists(path))
        {
            throw std::runtime_error(
                std::string("DasHost.exe not found at: ") + path);
        }
        return path;
    }

    /**
     * @brief 获取插件目录路径
     *
     * 从环境变量 DAS_PLUGIN_DIR 读取。
     * @return 插件目录路径
     * @throws std::runtime_error 如果环境变量未设置
     */
    inline std::string GetPluginDir()
    {
        const char* plugin_dir = std::getenv("DAS_PLUGIN_DIR");
        if (plugin_dir == nullptr || strlen(plugin_dir) == 0)
        {
            throw std::runtime_error(
                "DAS_PLUGIN_DIR environment variable is not set");
        }
        return plugin_dir;
    }

    /**
     * @brief 获取测试插件 JSON 清单路径
     * @param plugin_name 插件名称（如 "IpcTestPlugin1"）
     * @return JSON 文件路径
     * @throws std::runtime_error 如果环境变量未设置或文件不存在
     */
    inline std::string GetTestPluginJsonPath(const std::string& plugin_name)
    {
        std::filesystem::path json_path =
            std::filesystem::path{GetPluginDir()}
            / DAS_FMT_NS::format("{}.json", plugin_name);

        if (!std::filesystem::exists(json_path))
        {
            throw std::runtime_error(
                "Plugin JSON not found at: " + json_path.string());
        }
        return json_path.string();
    }

} // namespace IpcTestConfig

// ============================================================
// 测试夹具 - 用于真正启动进程的集成测试
// ============================================================

class IpcMultiProcessTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        host_exe_path_ = IpcTestConfig::GetDasHostPath();

        std::string msg =
            DAS_FMT_NS::format("DasHost path: {}", host_exe_path_);
        DAS_LOG_INFO(msg.c_str());
        // 初始化 MainProcessServer
        auto& server =
            DAS::Core::IPC::MainProcess::MainProcessServer::GetInstance();
        DasResult result = server.Initialize();
        if (DAS::IsFailed(result))
        {
            throw std::runtime_error(
                DAS_FMT_NS::format(
                    "Failed to initialize MainProcessServer: {}",
                    static_cast<int32_t>(result)));
        }

        DAS_LOG_INFO("MainProcessServer initialized");
    }

    void TearDown() override
    {
        // 关闭 MainProcessServer
        auto& server =
            DAS::Core::IPC::MainProcess::MainProcessServer::GetInstance();
        server.Shutdown();

        DAS_LOG_INFO("MainProcessServer shutdown");

        launcher_.Stop();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    /**
     * @brief 启动 Host 进程并注册到 ConnectionManager
     * @return DasResult 成功返回 DAS_S_OK
     */
    DasResult StartHostAndSetupRunLoop()
    {
        // 启动 Host 进程
        uint16_t  session_id = 0;
        DasResult result = launcher_.Start(
            host_exe_path_,
            session_id,
            IpcTestConfig::GetHostStartTimeoutMs());
        if (DAS::IsFailed(result))
        {
            DAS_LOG_ERROR("Failed to start host process");
            return result;
        }

        // 将 Transport 注册到 ConnectionManager
        auto transport = launcher_.ReleaseTransport();
        if (!transport)
        {
            DAS_LOG_ERROR("Failed to get transport from HostLauncher");
            return DAS_E_FAIL;
        }

        // 注册到 ConnectionManager（MainProcessServer.SendLoadPlugin 需要）
        // ConnectionManager 持有 Transport 的所有权
        auto& conn_manager = DAS::Core::IPC::ConnectionManager::GetInstance();
        result = conn_manager.RegisterHostTransport(
            session_id,
            std::move(transport),
            nullptr, // shm_pool
            nullptr  // run_loop (主进程不需要)
        );
        if (DAS::IsFailed(result))
        {
            DAS_LOG_ERROR("Failed to register transport to ConnectionManager");
            return result;
        }

        std::string msg = DAS_FMT_NS::format(
            "Transport registered to ConnectionManager, session_id={}",
            session_id);
        DAS_LOG_INFO(msg.c_str());
        return DAS_S_OK;
    }

    std::string                  host_exe_path_;
    DAS::Core::IPC::HostLauncher launcher_;
};
// ============================================================
// 辅助函数 - 发送 IPC 命令
// ============================================================

namespace IpcTestUtils
{
    /**
     * @brief 发送 LOAD_PLUGIN 命令并等待响应
     * @param runloop IPC RunLoop
     * @param plugin_json_path 插件 JSON 清单路径
     * @param out_object_id 输出：加载的对象 ID
     * @param timeout 超时时间（默认30秒）
     * @return DasResult 成功返回 DAS_S_OK
     */
    inline DasResult SendLoadPluginCommand(
        DAS::Core::IPC::IpcRunLoop* runloop,
        const std::string&          plugin_json_path,
        DAS::Core::IPC::ObjectId&   out_object_id,
        std::chrono::milliseconds   timeout = std::chrono::seconds(30))
    {
        using namespace DAS::Core::IPC;

        // 构建请求 payload: [uint16_t path_len][char path...]
        std::vector<uint8_t> payload;
        SerializeString(payload, plugin_json_path);

        // 构建消息头
        IPCMessageHeader header{};
        header.magic = IPCMessageHeader::MAGIC;
        header.version = IPCMessageHeader::CURRENT_VERSION;
        header.message_type = static_cast<uint8_t>(MessageType::REQUEST);
        header.header_flags = 0;
        header.call_id = 1;
        header.interface_id =
            static_cast<uint32_t>(IpcCommandType::LOAD_PLUGIN);
        header.method_id = 0;
        header.flags = 0;
        header.error_code = DAS_S_OK;
        header.body_size = static_cast<uint32_t>(payload.size());
        header.session_id = 1; // 主进程 session_id
        header.generation = 0;
        header.local_id = 0;

        // 使用 IpcRunLoop::SendRequest 发送请求并等待响应
        // 这支持可重入调用，可以处理 LoadPlugin 过程中的对象注册消息
        std::vector<uint8_t> response_body;
        DasResult            result = runloop->SendRequest(
            header,
            payload.data(),
            payload.size(),
            response_body,
            timeout);

        if (DAS::IsFailed(result))
        {
            return result;
        }

        // 解析响应: [object_id(8)][iid(16)][session_id(2)][version(2)]
        if (response_body.size() < 28)
        {
            return DAS_E_IPC_INVALID_MESSAGE_BODY;
        }

        size_t offset = 0;
        // 读取 object_id (8 bytes)
        out_object_id.session_id =
            static_cast<uint16_t>(response_body[offset])
            | (static_cast<uint16_t>(response_body[offset + 1]) << 8);
        offset += 2;
        out_object_id.generation =
            static_cast<uint16_t>(response_body[offset])
            | (static_cast<uint16_t>(response_body[offset + 1]) << 8);
        offset += 2;
        out_object_id.local_id =
            static_cast<uint32_t>(response_body[offset])
            | (static_cast<uint32_t>(response_body[offset + 1]) << 8)
            | (static_cast<uint32_t>(response_body[offset + 2]) << 16)
            | (static_cast<uint32_t>(response_body[offset + 3]) << 24);
        offset += 4;

        return DAS_S_OK;
    }
} // namespace IpcTestUtils
