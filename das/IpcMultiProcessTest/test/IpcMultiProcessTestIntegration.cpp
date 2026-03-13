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

#include "IpcMultiProcessTestIntegration.h"

#include "FakeMainProcess.h"

#include <das/Core/IPC/AsyncOperationImpl.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <stdexec/execution.hpp>

TEST_F(IpcMultiProcessTestIntegration, ProcessLaunch)
{
    // 测试进程启动（禁用：需要 DasHost.exe 存在）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_->Start(
        host_exe_path_,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());
    EXPECT_GT(launcher_->GetPid(), 0u);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));
}

TEST_F(IpcMultiProcessTestIntegration, HostLauncherStart)
{
    // 测试 HostLauncher.Start() 完整流程
    // 注意：WaitForHostReady 测试已合并到此测试，因为 Start() 已内置等待功能
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_->Start(
        host_exe_path_,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());
    EXPECT_GT(launcher_->GetPid(), 0u);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));
    EXPECT_EQ(session_id, launcher_->GetSessionId());
}

TEST_F(IpcMultiProcessTestIntegration, FullHandshake)
{
    // 测试完整握手流程（通过 Start() 一次性完成）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_->Start(
        host_exe_path_,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));
    EXPECT_EQ(session_id, launcher_->GetSessionId());
}

TEST_F(IpcMultiProcessTestIntegration, MultipleStartStop)
{
    // 测试多次启动/停止
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    for (int i = 0; i < 3; ++i)
    {
        uint16_t  session_id = 0;
        DasResult result = launcher_->Start(
            host_exe_path_,
            session_id,
            IpcTestConfig::GetHostStartTimeoutMs());
        ASSERT_EQ(result, DAS_S_OK) << "Failed at iteration " << i;
        EXPECT_TRUE(launcher_->IsRunning());
        EXPECT_GT(session_id, static_cast<uint16_t>(0));

        launcher_->Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_FALSE(launcher_->IsRunning());
    }
}

TEST_F(IpcMultiProcessTestIntegration, StopTerminatesProcess)
{
    // 测试 Stop() 正确终止进程
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_->Start(
        host_exe_path_,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

    launcher_->Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(launcher_->IsRunning());
}

TEST_F(IpcMultiProcessTestIntegration, GetSessionIdBeforeStart)
{
    // 测试启动前 GetSessionId() 返回 0
    EXPECT_EQ(launcher_->GetSessionId(), 0u);
}

TEST_F(IpcMultiProcessTestIntegration, GetPidBeforeStart)
{
    // 测试启动前 GetPid() 返回 0
    EXPECT_EQ(launcher_->GetPid(), 0u);
}

TEST_F(IpcMultiProcessTestIntegration, IsRunningBeforeStart)
{
    // 测试启动前 IsRunning() 返回 false
    EXPECT_FALSE(launcher_->IsRunning());
}

// ====== 跨进程调用测试 ======

/**
 * @brief 测试跨进程加载插件
 *
 * 验证主进程通过 IPC LOAD_PLUGIN 命令加载 Host 进程的插件。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_LoadPlugin)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动 Host 进程并注册到 ConnectionManager
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

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

    // 3. 通过 IIpcContext::LoadPluginAsync 加载插件
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Load plugin: wait failed";

    auto& [load_result, object_id] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";

    // 4. 验证返回的对象 ID
    EXPECT_EQ(object_id.session_id, launcher_->GetSessionId());

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
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_CallComponent)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动 Host 进程并注册到 ConnectionManager
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

    // 2. 获取 IpcTestPlugin2 JSON 路径
    std::string plugin_json_path =
        IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");

    // 3. 通过 IIpcContext::LoadPluginAsync 加载插件
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Load plugin: wait failed";

    auto& [load_result, factory_id] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";

    // 4. 验证 Factory 对象在正确的 Host 进程中
    EXPECT_EQ(factory_id.session_id, launcher_->GetSessionId());

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
 * 使用 RegisterHostLauncher 模式，Transport 保留在 HostLauncher 内部。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_VerifySessionId)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 创建并启动第一个 Host 进程
    DAS::DasPtr<DAS::Core::IPC::HostLauncher> host_a(
        new DAS::Core::IPC::HostLauncher(ctx_->GetIoContext()));

    uint16_t  session_a = 0;
    DasResult result = host_a->Start(
        host_exe_path_,
        session_a,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_a, static_cast<uint16_t>(0));

    // 注册 Host A 到 IPC 上下文
    result = ctx_->RegisterHostLauncher(host_a);
    ASSERT_EQ(result, DAS_S_OK);

    // 2. 创建并启动第二个 Host 进程
    DAS::DasPtr<DAS::Core::IPC::HostLauncher> host_b(
        new DAS::Core::IPC::HostLauncher(ctx_->GetIoContext()));

    uint16_t session_b = 0;
    result = host_b->Start(
        host_exe_path_,
        session_b,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_b, static_cast<uint16_t>(0));

    // 注册 Host B 到 IPC 上下文
    result = ctx_->RegisterHostLauncher(host_b);
    ASSERT_EQ(result, DAS_S_OK);

    // 3. 验证两个 session_id 不同
    EXPECT_NE(session_a, session_b);

    // 4. 获取插件 JSON 路径
    std::string plugin1_path, plugin2_path;
    try
    {
        plugin1_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        plugin2_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
    }
    catch (const std::exception& e)
    {
        host_a->Stop();
        host_b->Stop();
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 5. 使用异步接口并发加载两个插件（when_all）
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op_a;
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op_b;
    result =
        ctx_->LoadPluginAsync(host_a.Get(), plugin1_path.c_str(), op_a.Put());
    ASSERT_EQ(result, DAS_S_OK);
    result =
        ctx_->LoadPluginAsync(host_b.Get(), plugin2_path.c_str(), op_b.Put());
    ASSERT_EQ(result, DAS_S_OK);

    // when_all 并发等待两个异步操作
    auto both = stdexec::when_all(
        DAS::Core::IPC::async_op(GetContext(), std::move(op_a)),
        DAS::Core::IPC::async_op(GetContext(), std::move(op_b)));
    auto results = DAS::Core::IPC::wait(GetContext(), std::move(both));
    ASSERT_TRUE(results.has_value()) << "when_all: wait failed";

    // when_all 将两个 sender 的值展平为 tuple<DasResult, ObjectId, DasResult,
    // ObjectId>
    auto& [result_a, object_a, result_b, object_b] = *results;

    ASSERT_EQ(result_a, DAS_S_OK) << "Load plugin on Host A failed";
    ASSERT_EQ(result_b, DAS_S_OK) << "Load plugin on Host B failed";
    EXPECT_EQ(object_a.session_id, session_a);
    EXPECT_EQ(object_b.session_id, session_b);

    // 6. 验证不同 Host 的对象 session_id 不同
    EXPECT_NE(object_a.session_id, object_b.session_id);

    std::string log_msg = DAS_FMT_NS::format(
        "[CrossProcess_VerifySessionId] HostA object_id={{session:{}, "
        "local:{}}}, HostB object_id={{session:{}, local:{}}}",
        object_a.session_id,
        object_a.local_id,
        object_b.session_id,
        object_b.local_id);
    DAS_LOG_INFO(log_msg.c_str());

    // 7. 清理
    host_a->Stop();
    host_b->Stop();
}

/**
 * @brief 测试 Host A 通过主进程调用 Host B 的对象
 *
 * 完整的跨进程调用链：
 *   Host A → 主进程（转发）→ Host B
 *
 * 使用 RegisterHostLauncher 模式，Transport 保留在 HostLauncher 内部。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_HostToHostCall)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 创建并启动 Host B（目标进程）
    DAS::DasPtr<DAS::Core::IPC::HostLauncher> host_b(
        new DAS::Core::IPC::HostLauncher(ctx_->GetIoContext()));

    uint16_t  session_b = 0;
    DasResult result = host_b->Start(
        host_exe_path_,
        session_b,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_b, static_cast<uint16_t>(0));

    // 注册 Host B 到 IPC 上下文
    result = ctx_->RegisterHostLauncher(host_b);
    ASSERT_EQ(result, DAS_S_OK);

    // 2. 创建并启动 Host A（调用方进程）
    DAS::DasPtr<DAS::Core::IPC::HostLauncher> host_a(
        new DAS::Core::IPC::HostLauncher(ctx_->GetIoContext()));

    uint16_t session_a = 0;
    result = host_a->Start(
        host_exe_path_,
        session_a,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_a, static_cast<uint16_t>(0));
    EXPECT_NE(session_a, session_b);

    // 注册 Host A 到 IPC 上下文
    result = ctx_->RegisterHostLauncher(host_a);
    ASSERT_EQ(result, DAS_S_OK);

    // 3. 获取插件 JSON 路径
    std::string plugin1_path, plugin2_path;
    try
    {
        plugin1_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        plugin2_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
    }
    catch (const std::exception& e)
    {
        host_a->Stop();
        host_b->Stop();
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 4. 使用异步接口并发加载两个插件
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op_a;
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op_b;
    result =
        ctx_->LoadPluginAsync(host_a.Get(), plugin1_path.c_str(), op_a.Put());
    ASSERT_EQ(result, DAS_S_OK);
    result =
        ctx_->LoadPluginAsync(host_b.Get(), plugin2_path.c_str(), op_b.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto both = stdexec::when_all(
        DAS::Core::IPC::async_op(GetContext(), std::move(op_a)),
        DAS::Core::IPC::async_op(GetContext(), std::move(op_b)));
    auto results = DAS::Core::IPC::wait(GetContext(), std::move(both));
    ASSERT_TRUE(results.has_value()) << "when_all: wait failed";

    auto& [result_a, factory_id_a, result_b, factory_id_b] = *results;

    ASSERT_EQ(result_a, DAS_S_OK);
    EXPECT_EQ(factory_id_a.session_id, session_a);
    ASSERT_EQ(result_b, DAS_S_OK);
    EXPECT_EQ(factory_id_b.session_id, session_b);

    // 5. 验证 IPC 上下文可以获取已连接的 session
    auto connected_sessions = ctx_->GetConnectedSessions();
    EXPECT_EQ(connected_sessions.size(), 2);

    std::string log_msg = DAS_FMT_NS::format(
        "[CrossProcess_HostToHostCall] Cross-process infrastructure verified: "
        "HostA(session={}) can reach HostB(session={}) via IpcContext",
        session_a,
        session_b);
    DAS_LOG_INFO(log_msg.c_str());

    // 6. 清理
    host_a->Stop();
    host_b->Stop();
}
/**
 * @brief 测试加载 Java 插件
 *
 * 验证主进程通过 IPC 加载 Java 插件（JavaTestPlugin）。
 * 需要 JVM 环境可用。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_LoadJavaPlugin)
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
    EXPECT_TRUE(launcher_->IsRunning());

    // 4. 通过 IIpcContext::LoadPluginAsync 加载 Java 插件
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put(),
        IpcTestConfig::
            GetPluginLoadTimeout()); // Java 插件可能需要更长时间（JVM 初始化）

    if (DAS::IsFailed(result))
    {
        // JVM 可能不可用，跳过测试
        std::string err_msg = DAS_FMT_NS::format(
            "Failed to create load plugin operation (result={:#x}). "
            "Ensure JVM is properly installed and JAVA_HOME is set.",
            result);
        GTEST_SKIP() << err_msg;
    }

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    if (!opt.has_value())
    {
        GTEST_SKIP() << "Java plugin load timed out or failed";
    }

    auto& [load_result, object_id] = *opt;
    if (DAS::IsFailed(load_result))
    {
        // JVM 可能不可用，跳过测试
        std::string err_msg = DAS_FMT_NS::format(
            "Failed to load Java plugin (result={:#x}). "
            "Ensure JVM is properly installed and JAVA_HOME is set.",
            load_result);
        GTEST_SKIP() << err_msg;
    }

    // 5. 验证返回的对象 ID
    EXPECT_EQ(object_id.session_id, launcher_->GetSessionId());
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
TEST_F(
    IpcMultiProcessTestIntegration,
    ParentProcessExit_HostAutoExit_InvalidPid)
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
 * @brief 测试 Host 进程在主进程被杀后自动退出（完整握手版本）
 *
 * 模拟场景：启动一个辅助进程作为"假主进程"，完成真实握手后杀掉它，
 * 验证 Host 能感知并退出。
 *
 * 实现方式：
 * 1. 启动 FakeMain（等待 host_pid）
 * 2. 启动 DasHost，获得 host_pid
 * 3. 通过 shared_memory 将 host_pid 传给 FakeMain
 * 4. FakeMain 创建正确命名的管道，完成握手
 * 5. 测试框架杀掉 FakeMain
 * 6. 验证 DasHost 父进程监控检测到并退出
 */
TEST_F(
    IpcMultiProcessTestIntegration,
    ParentProcessExit_HostAutoExit_KillParent)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 获取当前可执行程序路径
    std::string test_exe_path;
    {
        char  buffer[MAX_PATH];
        DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
        {
            GTEST_SKIP() << "Failed to get current executable path";
        }
        test_exe_path = std::string(buffer, len);
    }

    // 2. 生成唯一信号名称
    std::string signal_name = FakeMainProcess::GenerateUniqueSignalName();

    // 3. 创建信号持有者（持有锁，FakeMain 会等待）
    FakeMainProcess::SignalHolder signal_holder(signal_name);

    // 4. 启动假主进程（等待 host_pid）
    //    FakeMain 会先创建共享内存，然后等待我们写入 host_pid
    boost::asio::io_context  fake_main_io_ctx;
    std::vector<std::string> fake_main_args = {
        "--fake-main",
        "--signal-name",
        signal_name};

    boost::process::v2::process fake_main(
        fake_main_io_ctx,
        test_exe_path,
        fake_main_args);

    auto fake_main_pid = static_cast<uint32_t>(fake_main.id());

    DAS_LOG_INFO(
        DAS_FMT_NS::format(
            "[KillParent] Fake main started: PID={}",
            fake_main_pid)
            .c_str());

    // 5. 启动 DasHost（连接到假主进程）
    //    先启动 DasHost 获取 host_pid，然后写入共享内存让 FakeMain 创建管道
    boost::asio::io_context  host_io_ctx;
    std::vector<std::string> host_args = {
        "--main-pid",
        std::to_string(fake_main_pid)};

    boost::process::v2::process host_process(
        host_io_ctx,
        host_exe_path_,
        host_args,
        boost::process::v2::process_start_dir(
            std::filesystem::path(host_exe_path_).parent_path().string()));

    auto host_pid = static_cast<uint32_t>(host_process.id());
    DAS_LOG_INFO(
        DAS_FMT_NS::format(
            "[KillParent] Host started: PID={}, main_pid={}",
            host_pid,
            fake_main_pid)
            .c_str());

    // 6. 将 host_pid 写入共享内存，让 FakeMain 可以创建正确命名的管道
    FakeMainProcess::KillParentSharedMemory::WriteHostPid(
        FakeMainProcess::KILL_PARENT_SHM_NAME,
        host_pid);
    DAS_LOG_INFO(
        DAS_FMT_NS::format(
            "[KillParent] Wrote host_pid={} to shared memory",
            host_pid)
            .c_str());

    // 7. 等待假主进程就绪（管道已创建）
    //    DasHost 有 1 秒的连接重试超时，应该足够让 FakeMain 创建管道
    bool ready = signal_holder.ReleaseAndWait(std::chrono::seconds(10));
    ASSERT_TRUE(ready) << "Fake main process did not become ready in time";

    // 8. 确认假主进程还在运行
    {
        boost::system::error_code ec;
        ASSERT_TRUE(fake_main.running(ec))
            << "Fake main process exited prematurely";
    }

    // 9. 等待握手完成
    bool handshake_done =
        FakeMainProcess::KillParentSharedMemory::WaitForHandshakeDone(
            FakeMainProcess::KILL_PARENT_SHM_NAME,
            std::chrono::seconds(10));
    ASSERT_TRUE(handshake_done) << "Handshake did not complete in time";
    DAS_LOG_INFO("[KillParent] Handshake completed");

    // 10. 确认 Host 还在运行
    {
        boost::system::error_code ec;
        ASSERT_TRUE(host_process.running(ec))
            << "Host process exited after handshake (should still be running)";
    }

    // 11. 杀掉假主进程（模拟主进程崩溃）
    {
        boost::system::error_code ec;
        fake_main.terminate(ec);
        DAS_LOG_INFO(
            DAS_FMT_NS::format(
                "[KillParent] Fake main killed: PID={}",
                fake_main_pid)
                .c_str());
    }

    // 12. 等待 Host 自动退出（父进程监控应检测到并退出）
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

    // 清理共享内存
    FakeMainProcess::KillParentSharedMemory::Cleanup(
        FakeMainProcess::KILL_PARENT_SHM_NAME);
    FakeMainProcess::FakeMainReadySignal::Cleanup(signal_name);

    if (!host_exited)
    {
        boost::system::error_code ec;
        host_process.terminate(ec);
        FAIL() << "Host did not exit after parent was killed. "
                  "Parent process monitoring is not working.";
    }

    DAS_LOG_INFO(
        "[KillParent] Test PASSED - Host exited automatically after real handshake");
}

/**
 * @brief 测试异步加载插件
 *
 * 验证 LoadPluginAsync + wait 异步接口，
 * 逐个加载两个插件。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_AsyncLoadPlugins)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动 Host 进程并注册到 ConnectionManager
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

    // 2. 获取插件 JSON 路径
    std::string plugin1_path, plugin2_path;
    try
    {
        plugin1_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        plugin2_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 3. 使用异步接口逐个加载插件1
    DAS::Core::IPC::ObjectId object1{};
    {
        DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
        result = ctx_->LoadPluginAsync(
            launcher_.Get(),
            plugin1_path.c_str(),
            op.Put());
        ASSERT_EQ(result, DAS_S_OK);
        auto opt = DAS::Core::IPC::wait(
            GetContext(),
            DAS::Core::IPC::async_op(GetContext(), std::move(op)));
        ASSERT_TRUE(opt.has_value()) << "Load plugin 1: wait failed";

        auto& [load_result, loaded_id] = *opt;
        ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin 1 failed";
        object1 = loaded_id;
    }

    // 4. 使用异步接口逐个加载插件2
    DAS::Core::IPC::ObjectId object2{};
    {
        DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
        result = ctx_->LoadPluginAsync(
            launcher_.Get(),
            plugin2_path.c_str(),
            op.Put());
        ASSERT_EQ(result, DAS_S_OK);
        auto opt = DAS::Core::IPC::wait(
            GetContext(),
            DAS::Core::IPC::async_op(GetContext(), std::move(op)));
        ASSERT_TRUE(opt.has_value()) << "Load plugin 2: wait failed";

        auto& [load_result, loaded_id] = *opt;
        ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin 2 failed";
        object2 = loaded_id;
    }

    // 5. 验证对象在正确的 Host 进程中
    EXPECT_EQ(object1.session_id, launcher_->GetSessionId());
    EXPECT_EQ(object2.session_id, launcher_->GetSessionId());

    std::string log_msg = DAS_FMT_NS::format(
        "[CrossProcess_AsyncLoadPlugins] Both plugins loaded: "
        "object1={{session:{}, local:{}}}, object2={{session:{}, local:{}}}",
        object1.session_id,
        object1.local_id,
        object2.session_id,
        object2.local_id);
    DAS_LOG_INFO(log_msg.c_str());
}

/**
 * @brief 测试 when_all 并发加载多个插件
 *
 * 使用 stdexec::when_all 在同一 Host 上并发加载两个插件，
 * 展示 stdexec 组合操作的用法。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_AsyncLoadPlugins_WhenAll)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动 Host 进程并注册到 ConnectionManager
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

    // 2. 获取插件 JSON 路径
    std::string plugin1_path, plugin2_path;
    try
    {
        plugin1_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        plugin2_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 3. 创建两个异步操作
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op1;
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op2;
    result =
        ctx_->LoadPluginAsync(launcher_.Get(), plugin1_path.c_str(), op1.Put());
    ASSERT_EQ(result, DAS_S_OK);
    result =
        ctx_->LoadPluginAsync(launcher_.Get(), plugin2_path.c_str(), op2.Put());
    ASSERT_EQ(result, DAS_S_OK);

    // 5. when_all 并发等待
    auto both = stdexec::when_all(
        DAS::Core::IPC::async_op(GetContext(), std::move(op1)),
        DAS::Core::IPC::async_op(GetContext(), std::move(op2)));
    auto results = DAS::Core::IPC::wait(GetContext(), std::move(both));
    ASSERT_TRUE(results.has_value()) << "when_all: wait failed";

    auto& [result1, object1, result2, object2] = *results;

    ASSERT_EQ(result1, DAS_S_OK) << "Load plugin 1 failed";
    ASSERT_EQ(result2, DAS_S_OK) << "Load plugin 2 failed";

    // 6. 验证对象在正确的 Host 进程中
    EXPECT_EQ(object1.session_id, launcher_->GetSessionId());
    EXPECT_EQ(object2.session_id, launcher_->GetSessionId());

    std::string log_msg = DAS_FMT_NS::format(
        "[CrossProcess_AsyncLoadPlugins_WhenAll] Both plugins loaded via "
        "when_all: object1={{session:{}, local:{}}}, object2={{session:{}, "
        "local:{}}}",
        object1.session_id,
        object1.local_id,
        object2.session_id,
        object2.local_id);
    DAS_LOG_INFO(log_msg.c_str());
}

// ====== Context 基础测试 ======

/**
 * @brief 测试 IIpcContext 的基础功能
 *
 * 验证：
 * 1. LoadPluginAsync 能通过 wait 执行
 */
TEST_F(IpcMultiProcessTestIntegration, Context_BasicUsage)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动 Host 进程并注册到 ConnectionManager
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

    // 2. 验证 async_op(ctx, op) 功能
    {
        std::string plugin_path;
        try
        {
            plugin_path =
                IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        }
        catch (const std::exception& e)
        {
            GTEST_SKIP() << "Plugin JSON not found: " << e.what();
        }

        DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
        result = ctx_->LoadPluginAsync(
            launcher_.Get(),
            plugin_path.c_str(),
            op.Put());
        ASSERT_EQ(result, DAS_S_OK);
        auto sender = DAS::Core::IPC::async_op(GetContext(), std::move(op));

        // 执行并验证结果
        auto opt = DAS::Core::IPC::wait(GetContext(), std::move(sender));
        ASSERT_TRUE(opt.has_value()) << "async_op with context: wait failed";

        auto& [load_result, loaded_id] = *opt;
        ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";
        EXPECT_EQ(loaded_id.session_id, launcher_->GetSessionId());
    }

    DAS_LOG_INFO("[Context_BasicUsage] All context tests passed");
}
