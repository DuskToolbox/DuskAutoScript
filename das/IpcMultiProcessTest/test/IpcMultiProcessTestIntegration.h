/**
 * @file IpcMultiProcessTestIntegration.h
 * @brief IPC 集成测试夹具
 *
 * 用于模拟真实的主进程启动和 IPC 过程。
 *
 * 规则：
 * - 禁止直接使用 MainProcessServer（常规操作）
 * - 必须通过 IIpcContext 进行操作
 * - 必须使用 async_op() 和 wait() 进行异步操作
 *
 * ============================================================================
 * 例外情况（仅限以下操作）：
 * ============================================================================
 * 为了设置测试环境（Transport 注册等），以下内部接口允许使用：
 * - MainProcessServer::GetInstance() - 仅用于初始化和清理
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
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/MainProcess/MainProcessServer.h>
#include <das/Core/IPC/SessionCoordinator.h>
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

        // 创建 IPC 上下文
        ctx_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared();
        if (!ctx_)
        {
            throw std::runtime_error("Failed to create IpcContext");
        }

        // 创建 HostLauncher（需要 io_context）
        DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher;
        DasResult result = ctx_->CreateHostLauncher(launcher.Put());
        if (DAS::IsFailed(result) || !launcher)
        {
            throw std::runtime_error("Failed to create HostLauncher");
        }
        // 将 IHostLauncher* 转换为 HostLauncher*（两者是同一个对象）
        auto* raw_launcher_ptr = static_cast<DAS::Core::IPC::HostLauncher*>(launcher.Get());
        launcher.Reset(); // DasPtr 放弃所有权
        launcher_.reset(raw_launcher_ptr);

        DAS_LOG_INFO("IpcContext and HostLauncher created");
    }

    void TearDown() override
    {
        launcher_.reset();
        ctx_.reset();

        // 重置 SessionCoordinator 的本地 session_id
        // 这样多个测试之间不会互相影响
        DAS::Core::IPC::SessionCoordinator::GetInstance().ResetLocalSessionId();

        DAS_LOG_INFO("IpcContext destroyed");
    }

    /**
     * @brief 获取 IPC Context（用于 stdexec 操作）
     *
     * 用户应通过此接口进行所有异步操作：
     * - async_op(GetContext(), op) - 创建异步操作 sender
     * - wait(GetContext(), sender) - 等待异步操作完成
     * - stdexec::schedule(GetContext()) - 获取 schedule sender
     */
    DAS::Core::IPC::MainProcess::IIpcContext& GetContext()
    {
        return *ctx_;
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
            *launcher_);
    }

protected:
    std::string host_exe_path_;

    // 使用 shared_ptr 存储
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ctx_;

    // HostLauncher 使用 unique_ptr（需要 io_context 构造）
    std::unique_ptr<DAS::Core::IPC::HostLauncher> launcher_;
};
