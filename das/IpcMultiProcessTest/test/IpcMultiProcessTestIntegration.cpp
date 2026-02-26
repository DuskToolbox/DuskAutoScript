/**
 * @file IpcMultiProcessTestIntegration.cpp
 * @brief IPC 多进程集成测试 - 真正启动进程的测试
 *
 * 这些测试会启动真实的 DasHost.exe 进程进行端到端测试。
 * 需要 DAS_HOST_EXE_PATH 环境变量指向 DasHost.exe。
 *
 * 测试场景：
 * 1. 进程启动与关闭
 * 2. 握手协议（Hello -> Welcome -> Ready -> ReadyAck）
 * 3. IPC 连接验证
 */

#include "IpcMultiProcessTestCommon.h"

// ====== 基础进程启动测试 ======

TEST_F(IpcMultiProcessTest, ProcessLaunch)
{
    // 测试进程启动（禁用：需要 DasHost.exe 存在）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_.Start(host_exe_path_, "", session_id, 10000);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_.IsRunning());
    EXPECT_GT(launcher_.GetPid(), 0u);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));
}

TEST_F(IpcMultiProcessTest, HostLauncherStart)
{
    // 测试 HostLauncher.Start() 完整流程
    // 注意：WaitForHostReady 测试已合并到此测试，因为 Start() 已内置等待功能
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_.Start(host_exe_path_, "", session_id, 10000);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_.IsRunning());
    EXPECT_GT(launcher_.GetPid(), 0u);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));
    EXPECT_EQ(session_id, launcher_.GetSessionId());
}

TEST_F(IpcMultiProcessTest, TransportAvailable)
{
    // 测试 IPC 传输接口可用
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_.Start(host_exe_path_, "", session_id, 10000);
    ASSERT_EQ(result, DAS_S_OK);

    auto* transport = launcher_.GetTransport();
    ASSERT_NE(transport, nullptr);
    EXPECT_TRUE(transport->IsConnected());
}

TEST_F(IpcMultiProcessTest, FullHandshake)
{
    // 测试完整握手流程（通过 Start() 一次性完成）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_.Start(host_exe_path_, "", session_id, 10000);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));
    EXPECT_EQ(session_id, launcher_.GetSessionId());
}

TEST_F(IpcMultiProcessTest, MultipleStartStop)
{
    // 测试多次启动/停止
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    for (int i = 0; i < 3; ++i)
    {
        uint16_t  session_id = 0;
        DasResult result =
            launcher_.Start(host_exe_path_, "", session_id, 10000);
        ASSERT_EQ(result, DAS_S_OK) << "Failed at iteration " << i;
        EXPECT_TRUE(launcher_.IsRunning());
        EXPECT_GT(session_id, static_cast<uint16_t>(0));

        launcher_.Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_FALSE(launcher_.IsRunning());
    }
}

TEST_F(IpcMultiProcessTest, StopTerminatesProcess)
{
    // 测试 Stop() 正确终止进程
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_.Start(host_exe_path_, "", session_id, 10000);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_.IsRunning());

    launcher_.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(launcher_.IsRunning());
}

TEST_F(IpcMultiProcessTest, GetSessionIdBeforeStart)
{
    // 测试启动前 GetSessionId() 返回 0
    EXPECT_EQ(launcher_.GetSessionId(), 0u);
}

TEST_F(IpcMultiProcessTest, GetPidBeforeStart)
{
    // 测试启动前 GetPid() 返回 0
    EXPECT_EQ(launcher_.GetPid(), 0u);
}

TEST_F(IpcMultiProcessTest, IsRunningBeforeStart)
{
    // 测试启动前 IsRunning() 返回 false
    EXPECT_FALSE(launcher_.IsRunning());
}
