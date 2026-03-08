/**
 * @file IpcTestHostHelper.h
 * @brief IPC 测试共享辅助函数 - Host 进程管理
 *
 * 被 Basic 和 Integration 测试共用。
 */

#pragma once

#include "IpcTestConfig.h"

#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/MainProcess/MainProcessServer.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#ifdef _WIN32
#include <das/Core/IPC/Win32AsyncIpcTransport.h>
#else
#include <das/Core/IPC/UnixAsyncIpcTransport.h>
#endif

namespace IpcTestHostHelper
{

    /**
     * @brief 启动 Host 进程并注册到 ConnectionManager
     *
     * @param host_exe_path Host 可执行文件路径
     * @param launcher HostLauncher 实例
     * @return DasResult 成功返回 DAS_S_OK
     *
     * @note 此函数需要访问 MainProcessServer 内部接口来设置 Transport
     */
    inline DasResult StartHostAndRegisterTransport(
        const std::string&            host_exe_path,
        DAS::Core::IPC::HostLauncher& launcher)
    {
        uint16_t  session_id = 0;
        DasResult result = launcher.Start(
            host_exe_path,
            session_id,
            IpcTestConfig::GetHostStartTimeoutMs());
        if (DAS::IsFailed(result))
        {
            DAS_LOG_ERROR("Failed to start host process");
            return result;
        }

        // 注册 HostLauncher 到 ConnectionManager
        // ConnectionManager 由 IpcRunLoop 持有，IpcRunLoop 由 MainProcessServer 持有
        auto& server = DAS::Core::IPC::MainProcess::MainProcessServer::GetInstance();
        auto* run_loop = server.GetRunLoop();
        if (!run_loop)
        {
            DAS_LOG_ERROR("Failed to get RunLoop from MainProcessServer");
            return DAS_E_FAIL;
        }

        auto* conn_manager = run_loop->GetConnectionManager();
        if (!conn_manager)
        {
            DAS_LOG_ERROR("Failed to get ConnectionManager from RunLoop");
            return DAS_E_FAIL;
        }

        result = conn_manager->RegisterHostLauncher(
            session_id,
            DAS::DasPtr<DAS::Core::IPC::IHostLauncher>(&launcher));
        if (DAS::IsFailed(result))
        {
            DAS_LOG_ERROR("Failed to register HostLauncher to ConnectionManager");
            return result;
        }

        std::string msg = DAS_FMT_NS::format(
            "HostLauncher registered, session_id={}",
            session_id);
        DAS_LOG_INFO(msg.c_str());

        return DAS_S_OK;
    }

} // namespace IpcTestHostHelper
