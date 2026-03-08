/**
 * @file IpcMultiProcessTestBasic.h
 * @brief IPC 基础测试夹具
 *
 * 用于测试 IPC 框架内部组件。
 *
 * 规则：
 * - 使用 IIpcContext 进行操作
 * - 使用 RegisterHostLauncher 注册 HostLauncher
 * - Transport 保留在 HostLauncher 内部
 *
 * 更新日志（08-04）：
 * - 移除对 MainProcessServer 单例的依赖
 * - 使用 IIpcContext 代替 MainProcessServer
 */

#pragma once

#include "IpcTestConfig.h"
#include "IpcTestHostHelper.h"

#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/DasApi.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>

class IpcMultiProcessTestBasic : public ::testing::Test
{
protected:
    void SetUp() override
    {
        host_exe_path_ = IpcTestConfig::GetDasHostPath();

        std::string msg =
            DAS_FMT_NS::format("DasHost path: {}", host_exe_path_);
        DAS_LOG_INFO(msg.c_str());

        // 创建 IPC 上下文（替代 MainProcessServer 单例）
        ctx_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared();
        if (!ctx_)
        {
            throw std::runtime_error("Failed to create IpcContext");
        }

        // 启动事件循环线程
        run_thread_ = std::thread([this]() {
            ctx_->Run();
        });

        DAS_LOG_INFO("IpcContext initialized");
    }

    void TearDown() override
    {
        ctx_->RequestStop();

        if (run_thread_.joinable())
        {
            run_thread_.join();
        }

        if (launcher_)
        {
            launcher_->Stop();
        }

        ctx_.reset();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        DAS_LOG_INFO("IpcContext shutdown");
    }

    DasResult StartHostAndSetupRunLoop()
    {
        // 创建 HostLauncher
        launcher_ = std::make_shared<DAS::Core::IPC::HostLauncher>(ctx_->GetIoContext());

        uint16_t session_id = 0;
        DasResult result = launcher_->Start(
            host_exe_path_,
            session_id,
            IpcTestConfig::GetHostStartTimeoutMs());
        if (DAS::IsFailed(result))
        {
            return result;
        }

        // 注册 HostLauncher 到 IPC 上下文
        result = ctx_->RegisterHostLauncher(launcher_);
        return result;
    }

protected:
    std::string                                            host_exe_path_;
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ctx_;
    std::shared_ptr<DAS::Core::IPC::HostLauncher>            launcher_;
    std::thread                                              run_thread_;
};
