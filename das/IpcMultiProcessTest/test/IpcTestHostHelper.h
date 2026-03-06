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
#include <das/Core/IPC/IpcTransport.h>
#include <das/Core/IPC/MainProcess/MainProcessServer.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>

namespace IpcTestHostHelper
{

    /**
     * @brief 启动 Host 进程并注册 Transport
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

        auto transport = launcher.ReleaseTransport();
        if (!transport)
        {
            DAS_LOG_ERROR("Failed to get transport from HostLauncher");
            return DAS_E_FAIL;
        }

        auto& conn_manager = DAS::Core::IPC::ConnectionManager::GetInstance();
        result = conn_manager.RegisterHostTransport(
            session_id,
            std::move(transport),
            nullptr,
            nullptr);
        if (DAS::IsFailed(result))
        {
            DAS_LOG_ERROR("Failed to register transport to ConnectionManager");
            return result;
        }

        std::string msg = DAS_FMT_NS::format(
            "Transport registered, session_id={}",
            session_id);
        DAS_LOG_INFO(msg.c_str());

        return DAS_S_OK;
    }

} // namespace IpcTestHostHelper
