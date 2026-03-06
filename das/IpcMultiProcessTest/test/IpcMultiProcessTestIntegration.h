/**
 * @file IpcMultiProcessTestIntegration.h
 * @brief IPC 集成测试夹具
 *
 * 用于模拟真实的主进程启动和 IPC 过程。
 *
 * 规则：
 * - ❌ 禁止直接使用 MainProcessServer（常规操作）
 * - ✅ 必须通过 IIpcContext 进行操作
 * - ✅ 必须使用 async_op() 和 wait() 进行异步操作
 *
 * ============================================================================
 * 例外情况（仅限以下操作）：
 * ============================================================================
 * 为了设置测试环境（Transport 注册等），以下内部接口允许使用：
 * - MainProcessServer::GetInstance() - 仅用于初始化和清理
 * - MainProcessServer::GetRunLoop()->SetTransportPtr() - 仅用于设置 Transport
 * - ConnectionManager - 仅用于注册 Transport
 *
 * 这些例外操作封装在 IpcTestHostHelper::StartHostAndRegisterTransport() 中。
 * 其他测试代码不得直接访问 MainProcessServer。
 * ============================================================================
 */

#pragma once

#include "IpcTestConfig.h"
#include "IpcTestHostHelper.h"

// Boost 头文件（用于进程启动测试）
#include <boost/asio/io_context.hpp>
#include <boost/process/v2/pid.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/system/error_code.hpp>

#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/MainProcess/MainProcessServer.h>
#include <das/DasApi.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>

class IpcMultiProcessTestIntegration : public ::testing::Test
{
protected:
    void SetUp() override
    {
        host_exe_path_ = IpcTestConfig::GetDasHostPath();

        std::string msg =
            DAS_FMT_NS::format("DasHost path: {}", host_exe_path_);
        DAS_LOG_INFO(msg.c_str());

        // 创建 IPC 上下文（shared_ptr 用于 Scheduler 包装）
        ctx_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared();
        if (!ctx_)
        {
            throw std::runtime_error("Failed to create IpcContext");
        }

        DAS_LOG_INFO("IpcContext created");
    }

    void TearDown() override
    {
        ctx_.reset();

        DAS_LOG_INFO("IpcContext destroyed");

        launcher_.Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    /**
     * @brief 获取 IPC Scheduler（用于 stdexec 操作）
     *
     * 用户应通过此接口进行所有异步操作：
     * - async_op(GetScheduler(), op) - 创建异步操作 sender
     * - wait(GetScheduler(), sender) - 等待异步操作完成
     * - stdexec::schedule(GetScheduler()) - 获取 schedule sender
     *
     * 如需访问 IIpcContext 的其他方法，使用 GetScheduler().context()
     */
    DAS::Core::IPC::MainProcess::Scheduler& GetScheduler()
    {
        if (!scheduler_)
        {
            scheduler_ = std::make_shared<DAS::Core::IPC::MainProcess::Scheduler>(
                DAS::Core::IPC::MainProcess::Scheduler(ctx_));
        }
        return *scheduler_;
    }

    /**
     * @brief 启动 Host 进程并设置 RunLoop 的 Transport
     *
     * 封装 IpcTestHostHelper::StartHostAndRegisterTransport
     */
    DasResult StartHostAndSetupRunLoop()
    {
        return IpcTestHostHelper::StartHostAndRegisterTransport(
            host_exe_path_,
            launcher_);
    }

protected:
    std::string host_exe_path_;

    // 使用 shared_ptr 存储，以便 Scheduler 包装
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ctx_;

    // Scheduler 包装器（延迟初始化）
    std::shared_ptr<DAS::Core::IPC::MainProcess::Scheduler> scheduler_;

    DAS::Core::IPC::HostLauncher launcher_;
};
