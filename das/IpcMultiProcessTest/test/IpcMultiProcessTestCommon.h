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
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/Host/HostConfig.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcTransport.h>
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
// 测试夹具 - 用于真正启动进程的集成测试
// ============================================================

class IpcMultiProcessTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        host_exe_path_ = GetDasHostPath();

        std::string msg =
            DAS_FMT_NS::format("DasHost path: {}", host_exe_path_);
        DAS_LOG_INFO(msg.c_str());
    }

    void TearDown() override
    {
        launcher_.Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::string GetDasHostPath()
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

    // WaitForHostReady 已移除 - HostLauncher.Start() 已内置此功能

    /**
     * @brief 获取测试插件 JSON 清单路径
     * @param plugin_name 插件名称（如 "IpcTestPlugin1"）
     * @return JSON 文件路径
     */
    std::string GetTestPluginJsonPath(const std::string& plugin_name)
    {
        // 从环境变量获取构建目录，或使用默认值
        const char* build_dir = std::getenv("DAS_BUILD_DIR");
        std::string base_dir =
            build_dir ? build_dir : "C:/vmbuild/bin/Debug/plugins";

        std::string json_path = base_dir + "/" + plugin_name + ".json";

        if (!std::filesystem::exists(json_path))
        {
            throw std::runtime_error("Plugin JSON not found at: " + json_path);
        }
        return json_path;
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
     * @param transport IPC 传输接口
     * @param plugin_json_path 插件 JSON 清单路径
     * @param out_object_id 输出：加载的对象 ID
     * @param timeout_ms 超时时间（毫秒）
     * @return DasResult 成功返回 DAS_S_OK
     */
    inline DasResult SendLoadPluginCommand(
        DAS::Core::IPC::IpcTransport* transport,
        const std::string&            plugin_json_path,
        DAS::Core::IPC::ObjectId&     out_object_id,
        uint32_t                      timeout_ms = 5000)
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

        // 发送请求
        DasResult result =
            transport->Send(header, payload.data(), payload.size());
        if (DAS::IsFailed(result))
        {
            return result;
        }

        // 接收响应
        IPCMessageHeader     response_header{};
        std::vector<uint8_t> response_body;
        result = transport->Receive(response_header, response_body, timeout_ms);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        // 检查响应错误码
        if (response_header.error_code != DAS_S_OK)
        {
            return static_cast<DasResult>(response_header.error_code);
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
