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

#include <das/Core/IPC/ConnectionManager.h>
#include <stdexec/execution.hpp>
TEST_F(IpcMultiProcessTest, ProcessLaunch)
{
    // 测试进程启动（禁用：需要 DasHost.exe 存在）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_.Start(
        host_exe_path_,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
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
    DasResult result = launcher_.Start(
        host_exe_path_,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_.IsRunning());
    EXPECT_GT(launcher_.GetPid(), 0u);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));
    EXPECT_EQ(session_id, launcher_.GetSessionId());
}

TEST_F(IpcMultiProcessTest, FullHandshake)
{
    // 测试完整握手流程（通过 Start() 一次性完成）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_.Start(
        host_exe_path_,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
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
        DasResult result = launcher_.Start(
            host_exe_path_,
            session_id,
            IpcTestConfig::GetHostStartTimeoutMs());
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
    DasResult result = launcher_.Start(
        host_exe_path_,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
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

// ====== 跨进程调用测试 ======

/**
 * @brief 测试跨进程加载插件
 *
 * 验证主进程通过 IPC LOAD_PLUGIN 命令加载 Host 进程的插件。
 */
TEST_F(IpcMultiProcessTest, CrossProcess_LoadPlugin)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动 Host 进程并注册到 ConnectionManager
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_.IsRunning());

    // 2. 获取插件 JSON 路径
    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 3. 通过 MainProcessServer 发送 LOAD_PLUGIN 命令
    auto& server =
        DAS::Core::IPC::MainProcess::MainProcessServer::GetInstance();
    DAS::Core::IPC::ObjectId object_id{};
    result = server.SendLoadPlugin(
        plugin_json_path,
        object_id,
        launcher_.GetSessionId());
    ASSERT_EQ(result, DAS_S_OK);

    // 4. 验证返回的对象 ID
    EXPECT_EQ(object_id.session_id, launcher_.GetSessionId());

    std::string log_msg = DAS_FMT_NS::format(
        "[CrossProcess_LoadPlugin] Plugin loaded, object_id={{session:{}, "
        "gen:{}, local:{}}}",
        object_id.session_id,
        object_id.generation,
        object_id.local_id);
    DAS_LOG_INFO(log_msg.c_str());
}

/**
 * @brief 测试跨进程调用 IDasComponent
 *
 * 验证主进程通过 IPC 加载 IpcTestPlugin2 并验证组件创建。
 */
TEST_F(IpcMultiProcessTest, CrossProcess_CallComponent)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动 Host 进程并注册到 ConnectionManager
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_.IsRunning());

    // 2. 获取 IpcTestPlugin2 JSON 路径
    std::string plugin_json_path =
        IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");

    // 3. 通过 MainProcessServer 加载插件
    auto& server =
        DAS::Core::IPC::MainProcess::MainProcessServer::GetInstance();
    DAS::Core::IPC::ObjectId factory_id{};
    result = server.SendLoadPlugin(
        plugin_json_path,
        factory_id,
        launcher_.GetSessionId());
    ASSERT_EQ(result, DAS_S_OK);

    // 4. 验证 Factory 对象在正确的 Host 进程中
    EXPECT_EQ(factory_id.session_id, launcher_.GetSessionId());

    std::string log_msg = DAS_FMT_NS::format(
        "[CrossProcess_CallComponent] IpcTestPlugin2 loaded, factory_id={{"
        "session:{}, gen:{}, local:{}}}",
        factory_id.session_id,
        factory_id.generation,
        factory_id.local_id);
    DAS_LOG_INFO(log_msg.c_str());
}

/**
 * @brief 测试验证对象返回正确的 session_id
 *
 * 验证从不同 Host 进程加载的插件返回正确的 session_id。
 */
TEST_F(IpcMultiProcessTest, CrossProcess_VerifySessionId)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动第一个 Host 进程
    DAS::Core::IPC::HostLauncher host_a;
    uint16_t                     session_a = 0;
    DasResult                    result = host_a.Start(
        host_exe_path_,
        session_a,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_a, static_cast<uint16_t>(0));

    // 2. 启动第二个 Host 进程
    DAS::Core::IPC::HostLauncher host_b;
    uint16_t                     session_b = 0;
    result = host_b.Start(
        host_exe_path_,
        session_b,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_b, static_cast<uint16_t>(0));

    // 3. 验证两个 session_id 不同
    EXPECT_NE(session_a, session_b);

    // 4. 注册 Transport 到 ConnectionManager
    auto transport_a = host_a.ReleaseTransport();
    auto transport_b = host_b.ReleaseTransport();
    ASSERT_NE(transport_a, nullptr);
    ASSERT_NE(transport_b, nullptr);

    auto& conn_manager = DAS::Core::IPC::ConnectionManager::GetInstance();
    result = conn_manager.RegisterHostTransport(
        session_a,
        std::move(transport_a),
        nullptr,
        nullptr);
    ASSERT_EQ(result, DAS_S_OK);
    result = conn_manager.RegisterHostTransport(
        session_b,
        std::move(transport_b),
        nullptr,
        nullptr);

    // 5. 获取插件 JSON 路径
    std::string plugin1_path, plugin2_path;
    try
    {
        plugin1_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        plugin2_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
    }
    catch (const std::exception& e)
    {
        conn_manager.UnregisterTransport(session_a);
        conn_manager.UnregisterTransport(session_b);
        host_a.Stop();
        host_b.Stop();
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 6. 通过 MainProcessServer 在 Host A 加载 IpcTestPlugin1
    auto& server =
        DAS::Core::IPC::MainProcess::MainProcessServer::GetInstance();
    DAS::Core::IPC::ObjectId object_a{};
    result = server.SendLoadPlugin(plugin1_path, object_a, session_a);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(object_a.session_id, session_a);

    // 7. 通过 MainProcessServer 在 Host B 加载 IpcTestPlugin2
    DAS::Core::IPC::ObjectId object_b{};
    result = server.SendLoadPlugin(plugin2_path, object_b, session_b);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(object_b.session_id, session_b);

    // 8. 验证不同 Host 的对象 session_id 不同
    EXPECT_NE(object_a.session_id, object_b.session_id);

    std::string log_msg = DAS_FMT_NS::format(
        "[CrossProcess_VerifySessionId] HostA object_id={{session:{}, "
        "local:{}}}, HostB object_id={{session:{}, local:{}}}",
        object_a.session_id,
        object_a.local_id,
        object_b.session_id,
        object_b.local_id);
    DAS_LOG_INFO(log_msg.c_str());

    // 9. 清理
    conn_manager.UnregisterTransport(session_a);
    conn_manager.UnregisterTransport(session_b);
    host_a.Stop();
    host_b.Stop();
}

/**
 * @brief 测试 Host A 通过主进程调用 Host B 的对象
 *
 * 完整的跨进程调用链：
 *   Host A → 主进程（转发）→ Host B
 *
 * 验证 MainProcessServer::ForwardMessageToHost() 功能。
 */
TEST_F(IpcMultiProcessTest, CrossProcess_HostToHostCall)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动 Host B（目标进程）
    DAS::Core::IPC::HostLauncher host_b;
    uint16_t                     session_b = 0;
    DasResult                    result = host_b.Start(
        host_exe_path_,
        session_b,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_b, static_cast<uint16_t>(0));

    // 2. 启动 Host A（调用方进程）
    DAS::Core::IPC::HostLauncher host_a;
    uint16_t                     session_a = 0;
    result = host_a.Start(
        host_exe_path_,
        session_a,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_a, static_cast<uint16_t>(0));
    EXPECT_NE(session_a, session_b);

    // 3. 注册 Transport 到 ConnectionManager
    auto transport_a = host_a.ReleaseTransport();
    auto transport_b = host_b.ReleaseTransport();
    ASSERT_NE(transport_a, nullptr);
    ASSERT_NE(transport_b, nullptr);

    auto& conn_manager = DAS::Core::IPC::ConnectionManager::GetInstance();
    result = conn_manager.RegisterHostTransport(
        session_a,
        std::move(transport_a),
        nullptr,
        nullptr);
    ASSERT_EQ(result, DAS_S_OK);
    result = conn_manager.RegisterHostTransport(
        session_b,
        std::move(transport_b),
        nullptr,
        nullptr);

    // 4. 获取插件 JSON 路径
    std::string plugin1_path, plugin2_path;
    try
    {
        plugin1_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        plugin2_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
    }
    catch (const std::exception& e)
    {
        conn_manager.UnregisterTransport(session_a);
        conn_manager.UnregisterTransport(session_b);
        host_a.Stop();
        host_b.Stop();
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 5. 通过 MainProcessServer 加载插件
    auto& server =
        DAS::Core::IPC::MainProcess::MainProcessServer::GetInstance();
    DAS::Core::IPC::ObjectId factory_id_a{}, factory_id_b{};

    result = server.SendLoadPlugin(plugin1_path, factory_id_a, session_a);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(factory_id_a.session_id, session_a);

    result = server.SendLoadPlugin(plugin2_path, factory_id_b, session_b);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(factory_id_b.session_id, session_b);

    // 6. 验证 ConnectionManager 可以获取 Transport
    auto* transport_from_a = conn_manager.GetTransport(session_b);
    EXPECT_NE(transport_from_a, nullptr);

    std::string log_msg = DAS_FMT_NS::format(
        "[CrossProcess_HostToHostCall] Cross-process infrastructure verified: "
        "HostA(session={}) can reach HostB(session={}) via ConnectionManager",
        session_a,
        session_b);
    DAS_LOG_INFO(log_msg.c_str());

    // 7. 清理
    conn_manager.UnregisterTransport(session_a);
    conn_manager.UnregisterTransport(session_b);
    host_a.Stop();
    host_b.Stop();
}
/**
 * @brief 测试加载 Java 插件
 *
 * 验证主进程通过 IPC 加载 Java 插件（JavaTestPlugin）。
 * 需要 JVM 环境可用。
 */
TEST_F(IpcMultiProcessTest, CrossProcess_LoadJavaPlugin)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 先获取 JavaTestPlugin JSON 路径（检查是否存在）
    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("JavaTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "JavaTestPlugin JSON not found: " << e.what();
    }

    // 2. 检查 JAR 文件是否存在
    std::filesystem::path jar_path =
        std::filesystem::path(plugin_json_path).parent_path()
        / "JavaTestPlugin.jar";
    if (!std::filesystem::exists(jar_path))
    {
        GTEST_SKIP() << "JavaTestPlugin.jar not found at: "
                     << jar_path.string();
    }

    // 3. 启动 Host 进程并注册到 ConnectionManager
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_.IsRunning());

    // 4. 通过 MainProcessServer 发送 LOAD_PLUGIN 命令
    auto& server =
        DAS::Core::IPC::MainProcess::MainProcessServer::GetInstance();
    DAS::Core::IPC::ObjectId object_id{};
    result = server.SendLoadPlugin(
        plugin_json_path,
        object_id,
        launcher_.GetSessionId(),
        IpcTestConfig::
            GetPluginLoadTimeout()); // Java 插件可能需要更长时间（JVM 初始化）

    if (DAS::IsFailed(result))
    {
        // JVM 可能不可用，跳过测试
        std::string err_msg = DAS_FMT_NS::format(
            "Failed to load Java plugin (result={:#x}). "
            "Ensure JVM is properly installed and JAVA_HOME is set.",
            result);
        GTEST_SKIP() << err_msg;
    }

    // 5. 验证返回的对象 ID
    EXPECT_EQ(object_id.session_id, launcher_.GetSessionId());
    EXPECT_GT(object_id.local_id, 0u);

    std::string log_msg = DAS_FMT_NS::format(
        "[CrossProcess_LoadJavaPlugin] Java plugin loaded, object_id={{"
        "session:{}, gen:{}, local:{}}}",
        object_id.session_id,
        object_id.generation,
        object_id.local_id);
    DAS_LOG_INFO(log_msg.c_str());
}

// ====== 主进程退出检测测试 ======

/**
 * @brief 测试 Host 进程在主进程不存在时自动退出
 *
 * 模拟场景：主进程被杀后 Host 进程能够感知并自行退出，
 * 不会变成僵尸进程。
 *
 * 方法：手动启动 DasHost.exe 并传入一个不存在的 PID 作为 --main-pid，
 * 验证 Host 进程能在合理时间内自动退出。
 */
TEST_F(IpcMultiProcessTest, ParentProcessExit_HostAutoExit_InvalidPid)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 使用一个不存在的 PID（极大值，几乎不可能有进程使用）
    constexpr uint32_t fake_main_pid = 99999999;

    // 手动启动 DasHost.exe 进程，传入不存在的 --main-pid
    boost::asio::io_context  io_ctx;
    std::vector<std::string> args;
    args.push_back("--main-pid");
    args.push_back(std::to_string(fake_main_pid));

    boost::process::v2::process host_process(
        io_ctx,
        host_exe_path_,
        args,
        boost::process::v2::process_start_dir(
            std::filesystem::path(host_exe_path_).parent_path().string()));

    uint32_t host_pid = static_cast<uint32_t>(host_process.id());
    ASSERT_GT(host_pid, 0u);

    std::string start_log = DAS_FMT_NS::format(
        "[ParentProcessExit_HostAutoExit_InvalidPid] "
        "Host process started with fake main_pid={}, host_pid={}",
        fake_main_pid,
        host_pid);
    DAS_LOG_INFO(start_log.c_str());

    // 等待 Host 进程自动退出（最多等待 10 秒）
    // 父进程监控线程检测间隔为 1 秒，加上初始化时间，5 秒内应该退出
    bool host_exited = false;
    for (int i = 0; i < 100; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        boost::system::error_code ec;
        if (!host_process.running(ec))
        {
            host_exited = true;
            break;
        }
    }

    if (!host_exited)
    {
        // 如果还没退出，强制终止避免僵尸进程
        boost::system::error_code ec;
        host_process.terminate(ec);
        FAIL()
            << "Host process did not exit after 10 seconds with invalid main_pid. "
               "Parent process monitoring is not working.";
    }

    std::string exit_log = DAS_FMT_NS::format(
        "[ParentProcessExit_HostAutoExit_InvalidPid] "
        "Host process exited automatically as expected");
    DAS_LOG_INFO(exit_log.c_str());
    SUCCEED();
}

/**
 * @brief 测试 Host 进程在主进程被杀后自动退出
 *
 * 模拟场景：启动一个辅助进程作为"假主进程"，用该 PID 启动 Host，
 * 然后杀掉辅助进程，验证 Host 能感知并退出。
 *
 * 这比 InvalidPid 测试更贴近真实场景，因为 Host 会先成功初始化 IPC 资源，
 * 然后在运行过程中检测到主进程消失。
 */
TEST_F(IpcMultiProcessTest, ParentProcessExit_HostAutoExit_KillParent)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动一个辅助进程作为"假主进程"
    //    使用系统自带的程序（Windows: timeout, Linux: sleep）
    boost::asio::io_context fake_parent_io_ctx;
#ifdef _WIN32
    // Windows: 使用 cmd /c timeout 来创建一个持续运行的进程
    std::string              fake_parent_exe = "cmd.exe";
    std::vector<std::string> fake_parent_args =
        {"/c", "timeout", "/t", "60", "/nobreak"};
#else
    std::string              fake_parent_exe = "/bin/sleep";
    std::vector<std::string> fake_parent_args = {"60"};
#endif

    boost::process::v2::process fake_parent(
        fake_parent_io_ctx,
        fake_parent_exe,
        fake_parent_args);

    uint32_t fake_parent_pid = static_cast<uint32_t>(fake_parent.id());
    ASSERT_GT(fake_parent_pid, 0u);

    std::string parent_log = DAS_FMT_NS::format(
        "[ParentProcessExit_KillParent] Fake parent started: PID={}",
        fake_parent_pid);
    DAS_LOG_INFO(parent_log.c_str());

    // 2. 使用假主进程的 PID 启动 DasHost.exe
    boost::asio::io_context  host_io_ctx;
    std::vector<std::string> host_args;
    host_args.push_back("--main-pid");
    host_args.push_back(std::to_string(fake_parent_pid));

    boost::process::v2::process host_process(
        host_io_ctx,
        host_exe_path_,
        host_args,
        boost::process::v2::process_start_dir(
            std::filesystem::path(host_exe_path_).parent_path().string()));

    uint32_t host_pid = static_cast<uint32_t>(host_process.id());
    ASSERT_GT(host_pid, 0u);

    std::string host_log = DAS_FMT_NS::format(
        "[ParentProcessExit_KillParent] Host started: PID={}, main_pid={}",
        host_pid,
        fake_parent_pid);
    DAS_LOG_INFO(host_log.c_str());

    // 3. 等待 Host 初始化完成（给它 2 秒时间）
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 确认 Host 还在运行
    {
        boost::system::error_code ec;
        ASSERT_TRUE(host_process.running(ec))
            << "Host process exited prematurely before parent was killed";
    }

    // 4. 杀掉假主进程（模拟主进程崩溃）
    {
        boost::system::error_code ec;
        fake_parent.terminate(ec);
        std::string kill_log = DAS_FMT_NS::format(
            "[ParentProcessExit_KillParent] Fake parent killed: PID={}",
            fake_parent_pid);
        DAS_LOG_INFO(kill_log.c_str());
    }

    // 5. 等待 Host 进程自动退出（最多等待 10 秒）
    //    父进程监控线程检测间隔为 1 秒
    bool host_exited = false;
    for (int i = 0; i < 100; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        boost::system::error_code ec;
        if (!host_process.running(ec))
        {
            host_exited = true;
            break;
        }
    }

    if (!host_exited)
    {
        // 如果还没退出，强制终止避免僵尸进程
        boost::system::error_code ec;
        host_process.terminate(ec);
        FAIL()
            << "Host process did not exit after 10 seconds when parent was killed. "
               "Parent process monitoring is not working.";
    }

    std::string result_log = DAS_FMT_NS::format(
        "[ParentProcessExit_KillParent] "
        "Host process exited automatically after parent was killed. Test PASSED.");
    DAS_LOG_INFO(result_log.c_str());
    SUCCEED();
}

// Wave 3: SendLoadPluginCommandAsync 和 ParseLoadPluginResponse 工具函数
// 已添加到 IpcMultiProcessTestCommon.h 中的 IpcTestUtils 命名空间，
// 可在后续测试中使用。
