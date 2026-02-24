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
 * 3. IPC 消息传输
 * 4. 插件加载
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

    DasResult result = launcher_.Launch(host_exe_path_);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_.IsRunning());
    EXPECT_GT(launcher_.GetPid(), 0u);
}

TEST_F(IpcMultiProcessTest, WaitForHostReady)
{
    // 测试等待 Host 进程 IPC 资源就绪（禁用：需要 DasHost.exe 存在）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    ASSERT_EQ(launcher_.Launch(host_exe_path_), DAS_S_OK);
    EXPECT_TRUE(WaitForHostReady(10000));
}

TEST_F(IpcMultiProcessTest, IpcClientConnect)
{
    // 测试 IPC 客户端连接（禁用：需要 DasHost.exe 存在）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    ASSERT_EQ(launcher_.Launch(host_exe_path_), DAS_S_OK);
    ASSERT_TRUE(WaitForHostReady(10000));

    DasResult result = client_.Connect(launcher_.GetPid());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(client_.IsConnected());
}

TEST_F(IpcMultiProcessTest, FullHandshake)
{
    // 测试完整握手流程（禁用：需要 DasHost.exe 存在）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    ASSERT_EQ(launcher_.Launch(host_exe_path_), DAS_S_OK);
    ASSERT_TRUE(WaitForHostReady(10000));
    ASSERT_EQ(client_.Connect(launcher_.GetPid()), DAS_S_OK);

    uint16_t  session_id = 0;
    DasResult result = client_.PerformFullHandshake(session_id, 10000);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));
}

// ====== LOAD_PLUGIN 命令测试 ======

TEST_F(IpcMultiProcessTest, LoadPlugin_Success)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    ASSERT_EQ(launcher_.Launch(host_exe_path_), DAS_S_OK);
    ASSERT_TRUE(WaitForHostReady(10000));
    ASSERT_EQ(client_.Connect(launcher_.GetPid()), DAS_S_OK);

    uint16_t  session_id = 0;
    DasResult result = client_.PerformFullHandshake(session_id, 10000);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));

    std::string cmake_binary_dir = std::getenv("CMAKE_BINARY_DIR")
                                       ? std::getenv("CMAKE_BINARY_DIR")
                                       : "C:/vmbuild";
    std::string config = std::getenv("CMAKE_BUILD_TYPE")
                             ? std::getenv("CMAKE_BUILD_TYPE")
                             : "Debug";
    std::string plugin_path =
        cmake_binary_dir + "/bin/" + config + "/plugins/IpcTestPlugin.json";

    if (!std::filesystem::exists(plugin_path))
    {
        GTEST_SKIP() << "IpcTestPlugin.json not found at: " << plugin_path;
    }

    result = client_.SendLoadPlugin(plugin_path);
    ASSERT_EQ(result, DAS_S_OK);

    DAS::Core::IPC::ObjectId object_id;
    uint32_t                 error_code = 0;
    result = client_.ReceiveLoadPluginResponse(object_id, error_code, 10000);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(error_code, DAS_S_OK);
    EXPECT_GT(object_id.local_id, 0u);
}

TEST_F(IpcMultiProcessTest, LoadPlugin_InvalidPath)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    ASSERT_EQ(launcher_.Launch(host_exe_path_), DAS_S_OK);
    ASSERT_TRUE(WaitForHostReady(10000));
    ASSERT_EQ(client_.Connect(launcher_.GetPid()), DAS_S_OK);

    uint16_t  session_id = 0;
    DasResult result = client_.PerformFullHandshake(session_id, 10000);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));

    std::string invalid_path = "C:/nonexistent/InvalidPlugin.json";

    result = client_.SendLoadPlugin(invalid_path);
    ASSERT_EQ(result, DAS_S_OK);

    DAS::Core::IPC::ObjectId object_id;
    uint32_t                 error_code = 0;
    result = client_.ReceiveLoadPluginResponse(object_id, error_code, 10000);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_NE(error_code, DAS_S_OK);
}
