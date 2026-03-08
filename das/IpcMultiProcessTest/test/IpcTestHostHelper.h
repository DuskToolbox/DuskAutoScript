/**
 * @file IpcTestHostHelper.h
 * @brief IPC 测试共享辅助函数 - Host 进程管理
 *
 * 被 Basic 和 Integration 测试共用。
 *
 * 新模式（08-04 重构后）：
 * - HostLauncher 由 shared_ptr 持有，Transport 永不转移
 * - 使用 IIpcContext::RegisterHostLauncher() 注册
 * - Transport 保留在 HostLauncher 内部，由 IpcRunLoop 通过 RegisterHostLauncher 启动接收
 */

#pragma once

#include "IpcTestConfig.h"

#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>

namespace IpcTestHostHelper
{

    /**
     * @brief 启动 Host 进程并注册 HostLauncher
     *
     * 新模式：HostLauncher 由 shared_ptr 持有，Transport 永不转移。
     * 注册 HostLauncher 后，Transport 将自动开始接收消息。
     *
     * @param host_exe_path Host 可执行文件路径
     * @param launcher HostLauncher 的 shared_ptr
     * @param ipc_context IPC 上下文
     * @return DasResult 成功返回 DAS_S_OK
     */
    inline DasResult StartHostAndRegisterLauncher(
        const std::string&                            host_exe_path,
        std::shared_ptr<DAS::Core::IPC::HostLauncher> launcher,
        DAS::Core::IPC::MainProcess::IIpcContext&     ipc_context)
    {
        uint16_t  session_id = 0;
        DasResult result = launcher->Start(
            host_exe_path,
            session_id,
            IpcTestConfig::GetHostStartTimeoutMs());
        if (DAS::IsFailed(result))
        {
            DAS_LOG_ERROR("Failed to start host process");
            return result;
        }

        // 注册 HostLauncher 到 IPC 上下文
        result = ipc_context.RegisterHostLauncher(launcher);
        if (DAS::IsFailed(result))
        {
            DAS_LOG_ERROR("Failed to register HostLauncher to IPC context");
            launcher->Stop();
            return result;
        }

        std::string msg = DAS_FMT_NS::format(
            "HostLauncher registered, session_id={}",
            session_id);
        DAS_LOG_INFO(msg.c_str());

        return DAS_S_OK;
    }

} // namespace IpcTestHostHelper
