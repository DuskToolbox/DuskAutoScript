/**
 * @file IpcMultiProcessTestIntegration.h
 * @brief IPC 集成测试夹具
 *
 * 用于模拟真实的主进程启动和 IPC 过程。
 *
 * 规则：
 * - 禁止直接使用 MainProcessServer（已删除）
 * - 必须通过 IIpcContext 进行操作
 * - 必须使用 async_op() 和 wait() 进行异步操作
 * - HostLauncher 使用 RegisterHostLauncher() 注册（新模式）
 *
 * 更新日志（08-04）：
 * - 移除对 MainProcessServer 的依赖
 * - 使用 IIpcContext::RegisterHostLauncher() 替代旧的 Transport 注册模式
 * - Transport 永不转移，保留在 HostLauncher 内部
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

#include <atomic>
#include <chrono>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>

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

        // 包装为 shared_ptr
        launcher_ = std::shared_ptr<DAS::Core::IPC::HostLauncher>(
            static_cast<DAS::Core::IPC::HostLauncher*>(raw_launcher),
            [](DAS::Core::IPC::HostLauncher* p) {
                if (p)
                {
                    p->Stop();
                    delete p;
                }
            });

        // 启动事件循环线程
        run_thread_ = std::thread([this]() {
            ctx_->Run();
        });

        // 等待事件循环启动
        for (int i = 0; i < 100 && !ctx_->GetIoContext().stopped(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void TearDown() override
    {
        // 停止事件循环
        ctx_->RequestStop();

        if (run_thread_.joinable())
        {
            run_thread_.join();
        }

        launcher_.reset();
        ctx_.reset();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
     * @brief 启动 Host 进程并注册 HostLauncher
     *
     * 使用新的 RegisterHostLauncher() 模式：
     * - Transport 保留在 HostLauncher 内部
     * - RegisterHostLauncher 自动启动接收循环
     */
    DasResult StartHostAndSetupRunLoop()
    {
        // 启动 Host 进程
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

        // 注册 HostLauncher 到 IPC 上下文
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
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ctx_;
    std::shared_ptr<DAS::Core::IPC::HostLauncher>            launcher_;
    std::thread                                              run_thread_;
};
