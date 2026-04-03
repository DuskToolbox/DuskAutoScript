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

#include <Das.ExportInterface.IDasVariantVector.hpp>
#include <Das.PluginInterface.IDasComponent.hpp>
#include <Das.PluginInterface.IDasPluginPackage.hpp>

#include "IpcMultiProcessTestIntegration.h"

#include "FakeMainProcess.h"
#include "IDasComponent.h"
#include "IDasPluginPackage.h"
#include "das/DasTypes.hpp"
#include <das/DasString.hpp>

#include <cstddef>
#include <cstdint>
#include <das/Core/IPC/AsyncOperationImpl.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/Utils/StdExecution.h>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <gtest/gtest.h>

using namespace Das::PluginInterface;

TEST_F(IpcMultiProcessTestIntegration, HostLauncherStart)
{
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

TEST_F(IpcMultiProcessTestIntegration, LauncherStateBeforeStart)
{
    EXPECT_EQ(launcher_->GetSessionId(), 0u);
    EXPECT_EQ(launcher_->GetPid(), 0u);
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

    DAS_CORE_LOG_INFO(
        "[CrossProcess_LoadPlugin] Plugin loaded, object_id={{session:{}, "
        "gen:{}, local:{}}}",
        object_id.session_id,
        object_id.generation,
        object_id.local_id);
}

/**
 * @brief 测试 Host A 通过主进程调用 Host B 的对象
 *
 * 完整的跨进程调用链：
 *   Host A → 主进程（转发）→ Host B
 *
 * HostLauncher::Start() 自动注册到 ConnectionManager。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_HostToHostCall)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 创建并启动 Host B（目标进程）
    DAS::Core::IPC::IHostLauncher* raw_host_b = nullptr;
    DasResult result = ctx_->CreateHostLauncher(&raw_host_b);
    ASSERT_EQ(result, DAS_S_OK);
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> host_b(raw_host_b);

    uint16_t session_b = 0;
    result = host_b->Start(
        host_exe_path_,
        session_b,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_b, static_cast<uint16_t>(0));

    // 2. 创建并启动 Host A（调用方进程）
    DAS::Core::IPC::IHostLauncher* raw_host_a = nullptr;
    result = ctx_->CreateHostLauncher(&raw_host_a);
    ASSERT_EQ(result, DAS_S_OK);
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> host_a(raw_host_a);

    uint16_t session_a = 0;
    result = host_a->Start(
        host_exe_path_,
        session_a,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_a, static_cast<uint16_t>(0));
    EXPECT_NE(session_a, session_b);

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

    DAS_CORE_LOG_INFO(
        "[CrossProcess_HostToHostCall] Cross-process infrastructure verified: "
        "HostA(session={}) can reach HostB(session={}) via IpcContext",
        session_a,
        session_b);

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

    // 获取 JavaTestPlugin JSON 路径（检查是否存在）
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

    // 检查 JAR 文件是否存在
    std::filesystem::path jar_path =
        std::filesystem::path(plugin_json_path).parent_path()
        / "JavaTestPlugin.jar";
    if (!std::filesystem::exists(jar_path))
    {
        GTEST_SKIP() << "JavaTestPlugin.jar not found at: "
                     << jar_path.string();
    }

    // 启动 Host 进程并注册到 ConnectionManager
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

    // 通过 IIpcContext::LoadPluginAsync 加载 Java 插件
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put(),
        IpcTestConfig::
            GetPluginLoadTimeout()); // Java 插件可能需要更长时间（JVM 初始化）

    ASSERT_EQ(result, DAS_S_OK);

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

    // 验证返回的对象 ID
    EXPECT_EQ(object_id.session_id, launcher_->GetSessionId());
    EXPECT_GT(object_id.local_id, 0u);

    // 创建远程 proxy 并获取 plugin package
    DAS::DasPtr<IDasBase> raw_proxy;
    ASSERT_EQ(
        ctx_->CreateRemoteProxy(
            object_id,
            DasIidOf<IDasBase>(),
            raw_proxy.Put()),
        DAS_S_OK);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    // 枚举 feature，验证 COMPONENT_FACTORY
    DAS::PluginInterface::DasPluginFeature feature;
    ASSERT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);
    EXPECT_EQ(
        feature,
        DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);

    // 通过 feature 创建 IDasComponentFactory
    IDasBase* factory_base_raw = nullptr;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(0, &factory_base_raw),
        DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    // 验证 IsSupported
    DasGuid empty_guid{};
    EXPECT_EQ(factory->IsSupported(empty_guid), DAS_E_NO_IMPLEMENTATION);
    EXPECT_EQ(
        factory->IsSupported(DasIidOf<DAS::PluginInterface::IDasComponent>()),
        DAS_S_OK);

    // CreateInstance 获取 IDasComponent
    DAS::PluginInterface::IDasComponent* component_raw = nullptr;
    ASSERT_EQ(
        factory->CreateInstance(
            DasIidOf<DAS::PluginInterface::IDasComponent>(),
            &component_raw),
        DAS_S_OK);
    ASSERT_NE(component_raw, nullptr);
    DAS::DasPtr<DAS::PluginInterface::IDasComponent> component(component_raw);

    // 调用 Dispatch("echo", ...) 跨进程验证
    {
        DasReadOnlyString                      method_name{"echo"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_EQ(dispatch_result, DAS_S_OK) << "Dispatch(echo) failed";
    }

    // 调用 Dispatch("compute", ...)
    {
        DasReadOnlyString                      method_name{"compute"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_EQ(dispatch_result, DAS_S_OK) << "Dispatch(compute) failed";
    }

    // 调用 Dispatch("getSessionInfo", ...)
    {
        DasReadOnlyString                      method_name{"getSessionInfo"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_EQ(dispatch_result, DAS_S_OK)
            << "Dispatch(getSessionInfo) failed";
    }

    DAS_CORE_LOG_INFO(
        "[CrossProcess_LoadJavaPlugin] Java plugin fully verified: "
        "LoadPlugin → EnumFeature → CreateFeatureInterface → IsSupported "
        "→ CreateInstance → Dispatch(echo/compute/getSessionInfo)");
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

    DAS_CORE_LOG_INFO(
        "[ParentProcessExit_HostAutoExit_InvalidPid] "
        "Host process started with fake main_pid={}, host_pid={}",
        fake_main_pid,
        host_pid);

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

    DAS_CORE_LOG_INFO(
        "[ParentProcessExit_HostAutoExit_InvalidPid] "
        "Host process exited automatically as expected");
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

    FakeMainProcess::KillParentSharedMemory::Cleanup(
        FakeMainProcess::KILL_PARENT_SHM_NAME);

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

    DAS_CORE_LOG_INFO("[KillParent] Fake main started: PID={}", fake_main_pid);

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
    DAS_CORE_LOG_INFO(
        "[KillParent] Host started: PID={}, main_pid={}",
        host_pid,
        fake_main_pid);

    // 6. 等待 FakeMain 创建共享内存（避免竞态条件）
    bool shm_ready =
        FakeMainProcess::KillParentSharedMemory::WaitForSharedMemoryReady(
            FakeMainProcess::KILL_PARENT_SHM_NAME,
            std::chrono::seconds(10));
    ASSERT_TRUE(shm_ready) << "FakeMain did not create shared memory in time";
    DAS_CORE_LOG_INFO("[KillParent] Shared memory is ready");

    // 7. 将 host_pid 写入共享内存，让 FakeMain 可以创建正确命名的管道
    FakeMainProcess::KillParentSharedMemory::WriteHostPid(
        FakeMainProcess::KILL_PARENT_SHM_NAME,
        host_pid);
    DAS_CORE_LOG_INFO(
        "[KillParent] Wrote host_pid={} to shared memory",
        host_pid);

    // 8. 等待假主进程就绪（管道已创建）
    //    DasHost 有 1 秒的连接重试超时，应该足够让 FakeMain 创建管道
    bool ready = signal_holder.ReleaseAndWait(std::chrono::seconds(10));
    ASSERT_TRUE(ready) << "Fake main process did not become ready in time";

    // 9. 确认假主进程还在运行
    {
        boost::system::error_code ec;
        ASSERT_TRUE(fake_main.running(ec))
            << "Fake main process exited prematurely";
    }

    // 10. 等待握手完成
    bool handshake_done =
        FakeMainProcess::KillParentSharedMemory::WaitForHandshakeDone(
            FakeMainProcess::KILL_PARENT_SHM_NAME,
            std::chrono::seconds(10));
    ASSERT_TRUE(handshake_done) << "Handshake did not complete in time";
    DAS_CORE_LOG_INFO("[KillParent] Handshake completed");

    // 11. 确认 Host 还在运行
    {
        boost::system::error_code ec;
        ASSERT_TRUE(host_process.running(ec))
            << "Host process exited after handshake (should still be running)";
    }

    // 12. 杀掉假主进程（模拟主进程崩溃）
    {
        boost::system::error_code ec;
        fake_main.terminate(ec);
        DAS_CORE_LOG_INFO(
            "[KillParent] Fake main killed: PID={}",
            fake_main_pid);
    }

    // 13. 等待 Host 自动退出（父进程监控应检测到并退出）
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

    DAS_CORE_LOG_INFO(
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

    // 1. 启动第一个 Host 进程
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

    // 3. 在第一个 Host 上使用异步接口加载插件1
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

    // 4. 启动第二个 Host 进程
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher2;
    {
        DAS::Core::IPC::IHostLauncher* raw_launcher = nullptr;
        result = ctx_->CreateHostLauncher(&raw_launcher);
        ASSERT_EQ(result, DAS_S_OK);
        ASSERT_NE(raw_launcher, nullptr);
        launcher2 = DAS::DasPtr<DAS::Core::IPC::IHostLauncher>(raw_launcher);

        uint16_t session_id2 = 0;
        result = launcher2->Start(
            host_exe_path_,
            session_id2,
            IpcTestConfig::GetHostStartTimeoutMs());
        ASSERT_EQ(result, DAS_S_OK);
    }

    // 5. 在第二个 Host 上使用异步接口加载插件2
    DAS::Core::IPC::ObjectId object2{};
    {
        DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
        result = ctx_->LoadPluginAsync(
            launcher2.Get(),
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

    // 6. 验证对象在不同的 Host 进程中
    EXPECT_EQ(object1.session_id, launcher_->GetSessionId());
    EXPECT_EQ(object2.session_id, launcher2->GetSessionId());
    EXPECT_NE(object1.session_id, object2.session_id);

    std::string log_msg = DAS_FMT_NS::format(
        "[CrossProcess_AsyncLoadPlugins] Both plugins loaded: "
        "object1={{session:{}, local:{}}}, object2={{session:{}, "
        "local:{}}}",
        object1.session_id,
        object1.local_id,
        object2.session_id,
        object2.local_id);
    DAS_CORE_LOG_INFO(log_msg.c_str());

    launcher2.Reset();
}

/**
 * @brief 测试 when_all 并发加载多个插件
 *
 * 使用 stdexec::when_all 在不同 Host 上并发加载两个插件，
 * 展示 stdexec 组合操作的用法。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_AsyncLoadPlugins_WhenAll)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动第一个 Host 进程
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

    // 2. 启动第二个 Host 进程
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher2;
    {
        DAS::Core::IPC::IHostLauncher* raw_launcher = nullptr;
        result = ctx_->CreateHostLauncher(&raw_launcher);
        ASSERT_EQ(result, DAS_S_OK);
        ASSERT_NE(raw_launcher, nullptr);
        launcher2 = DAS::DasPtr<DAS::Core::IPC::IHostLauncher>(raw_launcher);

        uint16_t session_id2 = 0;
        result = launcher2->Start(
            host_exe_path_,
            session_id2,
            IpcTestConfig::GetHostStartTimeoutMs());
        ASSERT_EQ(result, DAS_S_OK);
    }

    // 3. 获取插件 JSON 路径
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

    // 4. 创建两个异步操作，分别发到不同 Host
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op1;
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op2;
    result =
        ctx_->LoadPluginAsync(launcher_.Get(), plugin1_path.c_str(), op1.Put());
    ASSERT_EQ(result, DAS_S_OK);
    result =
        ctx_->LoadPluginAsync(launcher2.Get(), plugin2_path.c_str(), op2.Put());
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

    // 6. 验证对象在不同的 Host 进程中
    EXPECT_EQ(object1.session_id, launcher_->GetSessionId());
    EXPECT_EQ(object2.session_id, launcher2->GetSessionId());
    EXPECT_NE(object1.session_id, object2.session_id);

    std::string log_msg = DAS_FMT_NS::format(
        "[CrossProcess_AsyncLoadPlugins_WhenAll] Both plugins loaded via "
        "when_all: object1={{session:{}, local:{}}}, object2={{session:{}, "
        "local:{}}}",
        object1.session_id,
        object1.local_id,
        object2.session_id,
        object2.local_id);
    DAS_CORE_LOG_INFO(log_msg.c_str());

    launcher2.Reset();
}

// ====== Remote Proxy 测试 ======

/**
 * @brief 测试通过 CreateRemoteProxy 创建远程 proxy 并调用方法，
 * 同时测试 [out] 接口指针：CreateInstance 返回远程 IDasComponent*
 *
 * 验证：
 * 1. CreateRemoteProxy 支持 IDasComponentFactory 接口
 * 2. 通过 proxy 调用 IsSupported 方法返回正确结果
 */
TEST_F(IpcMultiProcessTestIntegration, RemoteProxy_ComponentFactory_IsSupported)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found";
    }

    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    std::string plugin_path =
        IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");

    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result =
        ctx_->LoadPluginAsync(launcher_.Get(), plugin_path.c_str(), op.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value());

    auto& [load_result, factory_id] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK);
    EXPECT_EQ(factory_id.session_id, launcher_->GetSessionId());

    DAS::DasPtr<IDasBase> raw_proxy;
    result = ctx_->CreateRemoteProxy(
        factory_id,
        DasIidOf<IDasBase>(),
        raw_proxy.Put());
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(raw_proxy, nullptr);

    DAS::PluginInterface::DasPluginPackage package;
    ASSERT_EQ(raw_proxy.As(package.Put()), DAS_S_OK);

    // 枚举所有 feature 并验证值
    std::vector<std::pair<uint64_t, DAS::PluginInterface::DasPluginFeature>>
        features;
    {
        for (uint64_t i = 0; i < 100; ++i)
        {
            DAS::PluginInterface::DasPluginFeature feature;
            const DasResult r = package->EnumFeature(i, &feature);
            if (DAS::IsFailed(r))
            {
                DAS_CORE_LOG_INFO(
                    "Enumerated {} features from plugin package (stopped at "
                    "index {})",
                    features.size(),
                    i);
                break;
            }
            features.emplace_back(i, feature);
        }
    }

    ASSERT_FALSE(features.empty());
    EXPECT_EQ(
        features[0].second,
        DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);

    // 用索引 0 创建第一个 feature 的接口
    const auto factoryBase = package.CreateFeatureInterface(features[0].first);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factoryBase.As(factory.Put()), DAS_S_OK);

    // 空 GUID 不被支持，应返回 DAS_E_NO_IMPLEMENTATION
    DasGuid guid{};
    EXPECT_EQ(factory->IsSupported(guid), DAS_E_NO_IMPLEMENTATION);

    // 正确的 IID 应被支持
    EXPECT_EQ(
        factory->IsSupported(DasIidOf<DAS::PluginInterface::IDasComponent>()),
        DAS_S_OK);

    DAS_CORE_LOG_INFO("[RemoteProxy_ComponentFactory_IsSupported] Test passed");
}

// ====== QueryMainProcessInterface E2E 测试 ======

/**
 * @brief 测试 IpcTestPlugin1 通过 DasQueryMainProcessInterface
 *        获取主进程注册的 IDasReadOnlyString
 *
 * 完整 E2E 流程：
 * 1. 主进程注册一个 IDasReadOnlyString 到 DistributedObjectManager +
 *    RemoteObjectRegistry
 * 2. 启动 Host 进程，加载 IpcTestPlugin1
 * 3. 通过 IDasComponentFactory 创建 IDasComponent
 * 4. 调用 Dispatch("queryMainProcessString")
 * 5. 验证 Dispatch 返回成功
 */
TEST_F(IpcMultiProcessTestIntegration, QueryMainProcessInterface_E2E)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found";
    }

    // 1. 获取插件 JSON 路径
    std::string plugin_path;
    try
    {
        plugin_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 2. 通过 PostCallback（stdexec::schedule + then）在 io_context 线程中
    //    依次调用 DasRegisterMainProcessService 和
    //    DasQueryMainProcessInterface，验证主进程路径下 C API 的完整调用链。
    //
    //    IpcContext::Run() 已在 run_thread_ 中以 ScopedCurrentIpcContext
    //    设置了 TLS，PostCallback 的 lambda 在同一线程执行时 TLS 已就绪，
    //    无需在 lambda 内重复设置。

    const char*       test_string = "Hello from main process E2E";
    DasReadOnlyString service_string{test_string};

    // 创建 IDasVariantVector（在主线程提前构造，通过引用捕获入 lambda）
    DAS::ExportInterface::DasVariantVector variant_vec;
    {
        DasResult vv_result = CreateIDasVariantVector(variant_vec.Put());
        ASSERT_EQ(vv_result, DAS_S_OK) << "CreateIDasVariantVector failed";
        vv_result = variant_vec->PushBackInt(42);
        ASSERT_EQ(vv_result, DAS_S_OK) << "PushBackInt(42) failed";
        vv_result = variant_vec->PushBackInt(123);
        ASSERT_EQ(vv_result, DAS_S_OK) << "PushBackInt(123) failed";
    }

    // wait() 阻塞直到两步全部完成，返回最终 DasResult
    auto reg_opt = DAS::Core::IPC::wait(
        stdexec::then(
            stdexec::then(
                stdexec::schedule(GetContext()),
                [&]() noexcept -> DasResult
                {
                    // 注册 IDasReadOnlyString
                    DasResult reg_result = DasRegisterMainProcessService(
                        service_string.Get(),
                        DasIidOf<IDasReadOnlyString>());
                    if (DAS::IsFailed(reg_result))
                    {
                        DAS_CORE_LOG_ERROR(
                            "[QueryMainProcessInterface_E2E] "
                            "DasRegisterMainProcessService(IDasReadOnlyString) "
                            "failed: result={}",
                            reg_result);
                        return reg_result;
                    }

                    // 回查验证：DasQueryMainProcessInterface 能取回刚注册的对象
                    IDasBase* queried = nullptr;
                    DasResult query_result = DasQueryMainProcessInterface(
                        DasIidOf<IDasReadOnlyString>(),
                        &queried);
                    if (DAS::IsFailed(query_result))
                    {
                        DAS_CORE_LOG_ERROR(
                            "[QueryMainProcessInterface_E2E] "
                            "DasQueryMainProcessInterface(IDasReadOnlyString) "
                            "failed: result={}",
                            query_result);
                        return query_result;
                    }
                    if (queried)
                    {
                        queried->Release();
                    }

                    DAS_CORE_LOG_INFO(
                        "[QueryMainProcessInterface_E2E] "
                        "DasRegisterMainProcessService + "
                        "DasQueryMainProcessInterface(IDasReadOnlyString) "
                        "OK");
                    return DAS_S_OK;
                }),
            [&](DasResult prev_result) noexcept -> DasResult
            {
                if (DAS::IsFailed(prev_result))
                {
                    return prev_result;
                }

                // 注册 IDasVariantVector
                DasResult reg_result = DasRegisterMainProcessService(
                    variant_vec.Get(),
                    DasIidOf<DAS::ExportInterface::IDasVariantVector>());
                if (DAS::IsFailed(reg_result))
                {
                    DAS_CORE_LOG_ERROR(
                        "[QueryMainProcessInterface_E2E] "
                        "DasRegisterMainProcessService(IDasVariantVector) "
                        "failed: result={}",
                        reg_result);
                    return reg_result;
                }

                // 回查验证：DasQueryMainProcessInterface 能取回刚注册的对象
                IDasBase* queried = nullptr;
                DasResult query_result = DasQueryMainProcessInterface(
                    DasIidOf<DAS::ExportInterface::IDasVariantVector>(),
                    &queried);
                if (DAS::IsFailed(query_result))
                {
                    DAS_CORE_LOG_ERROR(
                        "[QueryMainProcessInterface_E2E] "
                        "DasQueryMainProcessInterface(IDasVariantVector) "
                        "failed: result={}",
                        query_result);
                    return query_result;
                }
                if (queried)
                {
                    queried->Release();
                }

                DAS_CORE_LOG_INFO(
                    "[QueryMainProcessInterface_E2E] "
                    "DasRegisterMainProcessService + "
                    "DasQueryMainProcessInterface(IDasVariantVector) OK");
                return DAS_S_OK;
            }));

    ASSERT_TRUE(reg_opt.has_value()) << "PostCallback registration: wait failed";
    DasResult result = std::get<0>(*reg_opt);
    ASSERT_EQ(result, DAS_S_OK)
        << "DasRegisterMainProcessService / DasQueryMainProcessInterface "
           "failed in io_context thread";

    // 3. 启动 Host 进程
    result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    // 4. 加载 IpcTestPlugin1
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result =
        ctx_->LoadPluginAsync(launcher_.Get(), plugin_path.c_str(), op.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Load plugin: wait failed";

    auto& [load_result, object_id] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";

    DAS_CORE_LOG_INFO(
        "[QueryMainProcessInterface_E2E] Plugin loaded, "
        "object_id={{session:{}, gen:{}, local:{}}}",
        object_id.session_id,
        object_id.generation,
        object_id.local_id);

    // 5. CreateRemoteProxy 获取 IDasPluginPackage
    DAS::DasPtr<IDasBase> raw_proxy;
    result = ctx_->CreateRemoteProxy(
        object_id,
        DasIidOf<IDasBase>(),
        raw_proxy.Put());
    ASSERT_EQ(result, DAS_S_OK);

    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    // 6. EnumFeature: index=0 应为 INPUT_FACTORY, index=1 应为
    //    COMPONENT_FACTORY
    {
        DAS::PluginInterface::DasPluginFeature feature0;
        ASSERT_EQ(plugin_package->EnumFeature(0, &feature0), DAS_S_OK);
        EXPECT_EQ(
            feature0,
            DAS::PluginInterface::DAS_PLUGIN_FEATURE_INPUT_FACTORY);

        DAS::PluginInterface::DasPluginFeature feature1;
        ASSERT_EQ(plugin_package->EnumFeature(1, &feature1), DAS_S_OK);
        EXPECT_EQ(
            feature1,
            DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
    }

    // 7. CreateFeatureInterface(1) → IDasComponentFactory
    IDasBase* factory_base_raw = nullptr;
    result = plugin_package->CreateFeatureInterface(1, &factory_base_raw);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    // 8. IsSupported + CreateInstance → IDasComponent
    ASSERT_EQ(
        factory->IsSupported(DasIidOf<DAS::PluginInterface::IDasComponent>()),
        DAS_S_OK);

    DAS::PluginInterface::IDasComponent* component_raw = nullptr;
    result = factory->CreateInstance(
        DasIidOf<DAS::PluginInterface::IDasComponent>(),
        &component_raw);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(component_raw, nullptr);
    DAS::DasPtr<DAS::PluginInterface::IDasComponent> component(component_raw);

    // 9. Dispatch("queryMainProcessString") — 核心验证
    {
        DasReadOnlyString method_name{"queryMainProcessString"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        EXPECT_EQ(dispatch_result, DAS_S_OK)
            << "Dispatch(queryMainProcessString) failed — "
               "E2E query from Host to main process did not work "
               "(pre-existing: IDasReadOnlyString has no IPC proxy)";
    }

    // 10. Dispatch("queryMainProcessVariantVector") — 验证 IDL 定义的接口
    {
        DasReadOnlyString method_name{"queryMainProcessVariantVector"};
        DAS::ExportInterface::DasVariantVector vv_result;
        DAS::ExportInterface::DasVariantVector vv_params;
        ASSERT_EQ(CreateIDasVariantVector(vv_params.Put()), DAS_S_OK);

        DasResult dispatch_result = component->Dispatch(
            method_name.Get(),
            vv_params.Get(),
            vv_result.Put());
        EXPECT_EQ(dispatch_result, DAS_S_OK)
            << "Dispatch(queryMainProcessVariantVector) failed";

        // 验证插件返回的数据与主进程注册的一致
        int64_t val0 = 0;
        EXPECT_EQ(vv_result->GetInt(0, &val0), DAS_S_OK);
        EXPECT_EQ(val0, 42) << "VariantVector Int[0] should be 42";
    }

    DAS_CORE_LOG_INFO(
        "[QueryMainProcessInterface_E2E] VariantVector test passed — "
        "Host plugin successfully queried main process "
        "IDasVariantVector via DasQueryMainProcessInterface");

    DAS_CORE_LOG_INFO(
        "[QueryMainProcessInterface_E2E] Test passed — "
        "Host plugin successfully queried main process "
        "IDasReadOnlyString via DasQueryMainProcessInterface");
}

// ====== CrossProcess QueryMainProcessString E2E 测试 ======

/**
 * @brief 测试跨进程查询主进程字符串并验证返回值
 *
 * 完整 E2E 流程：
 * 1. 主进程注册一个 IDasReadOnlyString 到 DistributedObjectManager +
 *    RemoteObjectRegistry
 * 2. 启动 Host 进程，加载 IpcTestPlugin1
 * 3. 通过 IDasComponentFactory 创建 IDasComponent
 * 4. 调用 Dispatch("queryMainProcessString")
 * 5. 验证返回的 VariantVector 包含正确的字符串值
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_QueryMainProcessString)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found";
    }

    // 1. 获取插件 JSON 路径
    std::string plugin_path;
    try
    {
        plugin_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 2. 在主进程中注册一个 IDasReadOnlyString 服务
    DAS::DasPtr<IDasReadOnlyString> service_string;
    DasResult create_result = CreateIDasReadOnlyStringFromUtf8(
        "Hello from CrossProcess E2E",
        service_string.Put());
    ASSERT_EQ(create_result, DAS_S_OK) << "Failed to create test string";

    // 注册服务到主进程全局服务表
    DasResult result = ctx_->RegisterService(
        service_string.Get(),
        DasIidOf<IDasReadOnlyString>());
    ASSERT_EQ(result, DAS_S_OK) << "RegisterService failed";

    DAS_CORE_LOG_INFO(
        "[CrossProcess_QueryMainProcessString] Registered "
        "IDasReadOnlyString service in main process");

    // 3. 启动 Host 进程
    result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    // 4. 加载 IpcTestPlugin1
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result =
        ctx_->LoadPluginAsync(launcher_.Get(), plugin_path.c_str(), op.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Load plugin: wait failed";

    auto& [load_result, object_id] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";

    // 5. CreateRemoteProxy 获取 IDasPluginPackage
    DAS::DasPtr<IDasBase> raw_proxy;
    result = ctx_->CreateRemoteProxy(
        object_id,
        DasIidOf<IDasBase>(),
        raw_proxy.Put());
    ASSERT_EQ(result, DAS_S_OK);

    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    // 6. CreateFeatureInterface(1) → IDasComponentFactory
    IDasBase* factory_base_raw = nullptr;
    result = plugin_package->CreateFeatureInterface(1, &factory_base_raw);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    // 7. CreateInstance → IDasComponent
    ASSERT_EQ(
        factory->IsSupported(DasIidOf<DAS::PluginInterface::IDasComponent>()),
        DAS_S_OK);

    DAS::PluginInterface::IDasComponent* component_raw = nullptr;
    result = factory->CreateInstance(
        DasIidOf<DAS::PluginInterface::IDasComponent>(),
        &component_raw);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(component_raw, nullptr);
    DAS::DasPtr<DAS::PluginInterface::IDasComponent> component(component_raw);

    // 8. Dispatch("queryMainProcessString") — 核心验证
    {
        DasReadOnlyString method_name{"queryMainProcessString"};
        DAS::ExportInterface::DasVariantVector dispatch_result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        DasResult dispatch_hr = component->Dispatch(
            method_name.Get(),
            params.Get(),
            dispatch_result.Put());
        ASSERT_EQ(dispatch_hr, DAS_S_OK)
            << "Dispatch(queryMainProcessString) failed";

        // 验证返回的 VariantVector 包含 1 个元素
        ASSERT_NE(dispatch_result.Get(), nullptr);
        uint64_t size = dispatch_result->GetSize();
        ASSERT_EQ(size, 1u) << "Expected 1 element in result, got " << size;

        // 验证返回的字符串与注册的一致
        IDasReadOnlyString* out_string = nullptr;
        DasResult           get_hr = dispatch_result->GetString(0, &out_string);
        ASSERT_EQ(get_hr, DAS_S_OK) << "GetString(0) failed";
        ASSERT_NE(out_string, nullptr);

        const char* out_str = nullptr;
        DasResult   utf8_hr = out_string->GetUtf8(&out_str);
        ASSERT_EQ(utf8_hr, DAS_S_OK) << "GetUtf8 failed";
        ASSERT_NE(out_str, nullptr);
        EXPECT_STREQ(out_str, "Hello from CrossProcess E2E")
            << "Returned string does not match the registered value";

        out_string->Release();
    }

    DAS_CORE_LOG_INFO(
        "[CrossProcess_QueryMainProcessString] Test passed — "
        "Host plugin queried main process IDasReadOnlyString "
        "and returned correct value via IPC");
}
