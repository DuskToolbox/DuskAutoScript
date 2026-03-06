/**
 * @file IpcMultiProcessTestBasic.h
 * @brief IPC 基础测试夹具
 *
 * 用于测试 IPC 框架内部组件。
 *
 * 规则：
 * - ✅ 允许直接使用 MainProcessServer
 * - ✅ 允许访问内部实现细节
 * - ✅ 允许使用 IpcRunLoop
 */

#pragma once

#include "IpcTestConfig.h"
#include "IpcTestHostHelper.h"

#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/MainProcess/MainProcessServer.h>
#include <das/DasApi.h>
#include <gtest/gtest.h>
#include <string>

class IpcMultiProcessTestBasic : public ::testing::Test
{
protected:
    void SetUp() override
    {
        host_exe_path_ = IpcTestConfig::GetDasHostPath();

        std::string msg =
            DAS_FMT_NS::format("DasHost path: {}", host_exe_path_);
        DAS_LOG_INFO(msg.c_str());

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
        auto& server =
            DAS::Core::IPC::MainProcess::MainProcessServer::GetInstance();
        server.Shutdown();

        DAS_LOG_INFO("MainProcessServer shutdown");

        launcher_.Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    DasResult StartHostAndSetupRunLoop()
    {
        return IpcTestHostHelper::StartHostAndRegisterTransport(
            host_exe_path_,
            launcher_);
    }

protected:
    std::string                  host_exe_path_;
    DAS::Core::IPC::HostLauncher launcher_;
};
