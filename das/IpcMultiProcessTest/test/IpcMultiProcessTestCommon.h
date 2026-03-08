/**
 * @file IpcMultiProcessTestCommon.h
 * @brief IPC 多进程测试共享组件
 *
 * 包含用于 IPC 多进程测试的测试夹具。
 *
 * 更新日志（08-04）：
 * - 移除对 MainProcessServer 单例的依赖
 * - 使用 IIpcContext 作为主进程 IPC 上下文
 * - HostLauncher 注册模式改为 RegisterHostLauncher()
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
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
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

#include "IpcTestConfig.h"

// ============================================================
// 测试夹具 - 用于真正启动进程的集成测试
// ============================================================

class FpcMultiProcessTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        host_exe_path_ = IpcTestConfig::GetDasHostExePath();

        std::string msg =
            DAS_FMT_NS::format("DasHost path: {}", host_exe_path_);
        DAS_LOG_INFO(msg.c_str());

        // 创建 IPC 上下文（替代 MainProcessServer 单例）
        ctx_ = DAS::Core::IPC::MainProcess::CreateIpcContextEz();
        if (!ctx_)
        {
            throw std::runtime_error("Failed to create IpcContext");
        }

        // 创建 HostLauncher
        DAS::Core::IPC::IHostLauncher* raw_launcher = nullptr;
        DasResult result = ctx_->CreateHostLauncher(&raw_launcher);
        if (DAS::IsFailed(result) || !raw_launcher)
        {
            throw std::runtime_error(
                DAS_FMT_NS::format(
                    "Failed to create HostLauncher: {:#x}",
                    static_cast<uint32_t>(result)));
        }

        // 包装为 shared_ptr（使用正确的删除器）
        launcher_ = std::shared_ptr<DAS::Core::IPC::HostLauncher>(
            static_cast<DAS::Core::IPC::HostLauncher*>(raw_launcher),
            [](DAS::Core::IPC::HostLauncher* p) {
                if (p)
                {
                    p->Stop();
                    p->Release();
                }
            });

        DAS_LOG_INFO("IpcContext and HostLauncher created");
    }

    void TearDown() override
    {
        if (launcher_)
        {
            launcher_->Stop();
            launcher_.reset();
        }

        ctx_.reset();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    /**
     * @brief 获取 IIpcContext 引用
     *
     * 用于 async_op() 和 wait() 调用。
     */
    DAS::Core::IPC::MainProcess::IIpcContext& GetContext()
    {
        return *ctx_;
    }

    /**
     * @brief 启动 Host 进程并注册 HostLauncher
     *
     * 新模式：使用 IIpcContext::RegisterHostLauncher() 注册。
     * Transport 保留在 HostLauncher 内部，永不转移。
     *
     * @return DasResult 成功返回 DAS_S_OK
     */
    DasResult StartHostAndSetupRunLoop()
    {
        // 1. 启动 Host 进程
        uint16_t  session_id = 0;
        DasResult result = launcher_->Start(
            host_exe_path_,
            session_id,
            IpcTestConfig::GetHostStartTimeoutMs());
        if (DAS::IsFailed(result))
        {
            DAS_LOG_ERROR("Failed to start host process");
            return result;
        }

        // 2. 注册 HostLauncher 到 IPC 上下文
        // Transport 保留在 HostLauncher 内部，由 RegisterHostLauncher 启动接收
        result = ctx_->RegisterHostLauncher(launcher_);
        if (DAS::IsFailed(result))
        {
            DAS_LOG_ERROR("Failed to register HostLauncher to IPC context");
            launcher_->Stop();
            return result;
        }

        std::string msg = DAS_FMT_NS::format(
            "HostLauncher registered, session_id={}",
            session_id);
        DAS_LOG_INFO(msg.c_str());

        return DAS_S_OK;
    }

    std::string                                            host_exe_path_;
    DAS::Core::IPC::MainProcess::IpcContextPtr             ctx_;
    std::shared_ptr<DAS::Core::IPC::HostLauncher>          launcher_;
};

// ============================================================
// 辅助函数 - 发送 IPC 命令
// ============================================================

namespace IpcTestUtils
{

} // namespace IpcTestUtils
