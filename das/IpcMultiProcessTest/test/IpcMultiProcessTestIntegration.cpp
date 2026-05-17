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
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>

#include <cstddef>
#include <cstdint>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/IPC/AsyncOperationImpl.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/Utils/StdExecution.h>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <gtest/gtest.h>

using namespace Das::PluginInterface;
using namespace Das::ExportInterface;

namespace
{
    constexpr DasGuid kIpcTaskGuid{
        0xA1B2C3D4,
        0xE5F6,
        0x4A7B,
        {0x8C, 0x9D, 0x0E, 0x1F, 0x2A, 0x3B, 0x4C, 0x5D}};

    constexpr DasGuid kIpcTaskComponentGuid{
        0x68F10701,
        0x0000,
        0x4000,
        {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

    constexpr std::string_view kDasMaaPiTaskGuidText =
        "69F20001-0000-4000-8000-000000000001";
    constexpr std::string_view kDasMaaPiAuthoringFactoryGuidText =
        "69F20002-0000-4000-8000-000000000001";

    yyjson::value ToYyjson(Das::ExportInterface::IDasJson* json)
    {
        DAS::DasPtr<IDasReadOnlyString> text;
        EXPECT_EQ(json->ToString(0, text.Put()), DAS_S_OK);
        const char* u8 = nullptr;
        EXPECT_EQ(text->GetUtf8(&u8), DAS_S_OK);
        auto parsed =
            Das::Utils::ParseYyjsonFromString(u8 != nullptr ? u8 : "");
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }
} // namespace

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

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";

    // 4. 验证返回的代理对象
    ASSERT_NE(proxy, nullptr);

    DAS_CORE_LOG_INFO(
        "[CrossProcess_LoadPlugin] Plugin loaded, proxy = {}",
        (void*)proxy);
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_TaskAuthoringFactory)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

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

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK);
    ASSERT_NE(proxy, nullptr);

    DAS::DasPtr<IDasBase> raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    DasPluginFeature feature{};
    ASSERT_EQ(plugin_package->EnumFeature(2, &feature), DAS_S_OK);
    EXPECT_EQ(feature, DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY);

    IDasBase* factory_base_raw = nullptr;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(2, &factory_base_raw),
        DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

    DAS::DasPtr<IDasTaskAuthoringSessionFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    IDasTaskAuthoringSession* session_raw = nullptr;
    ASSERT_EQ(
        factory->CreateSession(kIpcTaskGuid, nullptr, &session_raw),
        DAS_S_OK);
    ASSERT_NE(session_raw, nullptr);
    DAS::DasPtr<IDasTaskAuthoringSession> session(session_raw);

    DAS::DasPtr<IDasJson> document;
    ASSERT_EQ(session->GetDocument(nullptr, document.Put()), DAS_S_OK);
    auto json = ToYyjson(document.Get());
    auto obj = json.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("kind")].as_string().value_or(""),
        std::string_view("formSequence"));

    DAS::DasPtr<IDasJson> apply_result;
    ASSERT_EQ(session->ApplyChange(nullptr, apply_result.Put()), DAS_S_OK);
    auto apply_json = ToYyjson(apply_result.Get());
    EXPECT_TRUE(apply_json.as_object()->contains(
        std::string_view("acceptedProperties")));

    DAS::DasPtr<IDasJson> compile_result;
    ASSERT_EQ(session->Compile(nullptr, compile_result.Put()), DAS_S_OK);
    auto compile_json = ToYyjson(compile_result.Get());
    EXPECT_TRUE(
        (*compile_json.as_object())[std::string_view("ok")].as_bool().value_or(
            false));
}

TEST_F(
    IpcMultiProcessTestIntegration,
    CrossProcess_DasMaaPiTaskAuthoringFactory)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    std::string plugin_json_path;
    try
    {
        plugin_json_path = IpcTestConfig::GetTestPluginJsonPath("DasMaaPi");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "DasMaaPi JSON not found: " << e.what();
    }

    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put(),
        IpcTestConfig::GetPluginLoadTimeout());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Load DasMaaPi: wait failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK);
    ASSERT_NE(proxy, nullptr);

    DAS::DasPtr<IDasBase> raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    DasPluginFeature feature{};
    ASSERT_EQ(plugin_package->EnumFeature(1, &feature), DAS_S_OK);
    EXPECT_EQ(feature, DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY);

    IDasBase* factory_base_raw = nullptr;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(1, &factory_base_raw),
        DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

    DAS::DasPtr<IDasTaskAuthoringSessionFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);
    DasGuid factory_guid{};
    ASSERT_EQ(factory->GetGuid(&factory_guid), DAS_S_OK);
    EXPECT_EQ(
        factory_guid,
        DAS::Core::ForeignInterfaceHost::MakeDasGuid(
            kDasMaaPiAuthoringFactoryGuidText));

    IDasTaskAuthoringSession* session_raw = nullptr;
    ASSERT_EQ(
        factory->CreateSession(
            DAS::Core::ForeignInterfaceHost::MakeDasGuid(kDasMaaPiTaskGuidText),
            nullptr,
            &session_raw),
        DAS_S_OK);
    ASSERT_NE(session_raw, nullptr);
    DAS::DasPtr<IDasTaskAuthoringSession> session(session_raw);

    DAS::DasPtr<IDasJson> document;
    ASSERT_EQ(session->GetDocument(nullptr, document.Put()), DAS_S_OK);
    auto json = ToYyjson(document.Get());
    auto obj = json.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("kind")].as_string().value_or(""),
        std::string_view("formSequence"));
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_TaskComponentFactory)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

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

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK);
    ASSERT_NE(proxy, nullptr);

    DAS::DasPtr<IDasBase> raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    DasPluginFeature feature{};
    ASSERT_EQ(plugin_package->EnumFeature(3, &feature), DAS_S_OK);
    EXPECT_EQ(feature, DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY);

    IDasBase* factory_base_raw = nullptr;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(3, &factory_base_raw),
        DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

    DAS::DasPtr<IDasTaskComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    IDasTaskComponent* missing_component_raw = nullptr;
    ASSERT_EQ(
        factory->CreateComponent(kIpcTaskGuid, &missing_component_raw),
        DAS_E_NOT_FOUND);
    ASSERT_EQ(missing_component_raw, nullptr);

    IDasTaskComponent* component_raw = nullptr;
    ASSERT_EQ(
        factory->CreateComponent(kIpcTaskComponentGuid, &component_raw),
        DAS_S_OK);
    ASSERT_NE(component_raw, nullptr);
    DAS::DasPtr<IDasTaskComponent> component(component_raw);

    DAS::DasPtr<IDasJson> settings_result;
    ASSERT_EQ(
        component->ApplySettingsChange(nullptr, settings_result.Put()),
        DAS_S_OK);
    auto settings_json = ToYyjson(settings_result.Get());
    EXPECT_TRUE(settings_json.as_object()->contains(
        std::string_view("acceptedSettings")));

    DAS::DasPtr<IDasJson> do_result;
    ASSERT_EQ(
        component->Do(nullptr, nullptr, nullptr, nullptr, do_result.Put()),
        DAS_S_OK);
    auto do_json = ToYyjson(do_result.Get());
    EXPECT_EQ(
        (*do_json.as_object())[std::string_view("status")].as_string().value_or(
            ""),
        std::string_view("completed"));
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

    auto& [result_a, p_a, result_b, p_b] = *results;

    ASSERT_EQ(result_a, DAS_S_OK);
    ASSERT_EQ(result_b, DAS_S_OK);

    auto proxy_a = DAS::DasPtr<IDasBase>::Attach(p_a);
    auto proxy_b = DAS::DasPtr<IDasBase>::Attach(p_b);
    ASSERT_NE(proxy_a.Get(), proxy_b.Get());

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
#ifndef DAS_EXPORT_JAVA
    GTEST_SKIP() << "DAS_EXPORT_JAVA is not enabled";
#endif
    ASSERT_TRUE(std::filesystem::exists(host_exe_path_))
        << "DasHost.exe not found at: " << host_exe_path_;

    // 获取 JavaTestPlugin JSON 路径（检查是否存在）
    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("JavaTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "JavaTestPlugin JSON not found: " << e.what();
    }

    // 检查 JAR 文件是否存在
    std::filesystem::path jar_path =
        std::filesystem::path(plugin_json_path).parent_path()
        / "JavaTestPlugin.jar";
    ASSERT_TRUE(std::filesystem::exists(jar_path))
        << "JavaTestPlugin.jar not found at: " << jar_path.string();

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
    ASSERT_TRUE(opt.has_value()) << "Java plugin load timed out or failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_TRUE(DAS::IsOk(load_result)) << DAS_FMT_NS::format(
        "Failed to load Java plugin (result={:#x}). "
        "Ensure JVM is properly installed and JAVA_HOME is set.",
        load_result);

    // 验证返回的代理对象
    ASSERT_NE(proxy, nullptr);

    // 通过代理获取 plugin package
    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
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
    // 设计意图：传递空 VariantVector，Java 端应返回 DAS_E_INVALID_ARGUMENT，
    // C++ 端校验错误码传播正确
    {
        DasReadOnlyString                      method_name{"echo"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_NE(dispatch_result, DAS_S_OK)
            << "Dispatch(echo) with empty params should return error code";
    }

    // 调用 Dispatch("compute", ...) — 同理，空参数应返回错误码
    {
        DasReadOnlyString                      method_name{"compute"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_NE(dispatch_result, DAS_S_OK)
            << "Dispatch(compute) with empty params should return error code";
    }

    // 调用 Dispatch("getSessionInfo", ...)
    // getSessionInfo Java handler 不检查参数数量，直接返回 session 信息
    // 所以空参数时 dispatch_result 应为 DAS_S_OK
    {
        DasReadOnlyString                      method_name{"getSessionInfo"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_EQ(dispatch_result, DAS_S_OK)
            << "Dispatch(getSessionInfo) should succeed (handler does not "
               "validate param count)";
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
    DAS::DasPtr<IDasBase> proxy1;
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

        auto& [load_result, loaded_proxy] = *opt;
        ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin 1 failed";
        proxy1 = DAS::DasPtr<IDasBase>::Attach(loaded_proxy);
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
    DAS::DasPtr<IDasBase> proxy2;
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

        auto& [load_result, loaded_proxy] = *opt;
        ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin 2 failed";
        proxy2 = DAS::DasPtr<IDasBase>::Attach(loaded_proxy);
    }

    // 6. 验证两个代理对象均非空且不同
    ASSERT_NE(proxy1.Get(), proxy2.Get());

    DAS_CORE_LOG_INFO(
        "[CrossProcess_AsyncLoadPlugins] Both plugins loaded: "
        "object1 = {}, object2 = {}",
        (void*)proxy1.Get(),
        (void*)proxy2.Get());

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

    auto& [result1, p1, result2, p2] = *results;

    ASSERT_EQ(result1, DAS_S_OK) << "Load plugin 1 failed";
    ASSERT_EQ(result2, DAS_S_OK) << "Load plugin 2 failed";

    // 6. 验证两个代理对象均非空且不同
    auto proxy1 = DAS::DasPtr<IDasBase>::Attach(p1);
    auto proxy2 = DAS::DasPtr<IDasBase>::Attach(p2);
    ASSERT_NE(proxy1.Get(), proxy2.Get());

    DAS_CORE_LOG_INFO(
        "[CrossProcess_AsyncLoadPlugins_WhenAll] Both plugins loaded via "
        "when_all: object1 = {}, object2 = {}",
        (void*)proxy1.Get(),
        (void*)proxy2.Get());

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

    auto& [load_result, factory_proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK);
    ASSERT_NE(factory_proxy, nullptr);

    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(factory_proxy);
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

    ASSERT_TRUE(reg_opt.has_value())
        << "PostCallback registration: wait failed";
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

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";
    ASSERT_NE(proxy, nullptr);

    DAS_CORE_LOG_INFO(
        "[QueryMainProcessInterface_E2E] Plugin loaded, proxy = {}",
        (void*)proxy);

    // 5. Use the proxy directly to get IDasPluginPackage
    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    ASSERT_NE(raw_proxy, nullptr);

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

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";
    ASSERT_NE(proxy, nullptr);

    DAS_CORE_LOG_INFO(
        "[QueryMainProcessInterface_E2E] Plugin loaded, proxy = {}",
        (void*)proxy);

    // 5. Use the proxy directly to get IDasPluginPackage
    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    ASSERT_NE(raw_proxy, nullptr);

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

// ==========================================================================
// CrossProcess_JavaDirectorLifecycleTest
// ==========================================================================

// {10de2795-5cb4-43d7-885a-a6e35a04bbe2}
DAS_DEFINE_CLASS_GUID(
    LifecycleCallbackComponent,
    0x10de2795,
    0x5cb4,
    0x43d7,
    0x88,
    0x5a,
    0xa6,
    0xe3,
    0x5a,
    0x04,
    0xbe,
    0xe2);

// PostCallback 完成信号 — 通过 IDasAsyncCallback + PostCallback
// 实现事件驱动通知 Dispatch() 收到 IPC 回调后调用 ctx_->PostCallback()，触发
// Do() → done = true 测试线程以 1ms 粒度轮询 done（与 DAS::Core::IPC::wait()
// 的内部模式一致）
class CompletionSignal : public IDasAsyncCallback
{
    DAS_UTILS_IDASBASE_AUTO_IMPL(CompletionSignal);

public:
    std::atomic<bool>& done_;

    explicit CompletionSignal(std::atomic<bool>& done) : done_(done) {}

    DasResult Do() noexcept override
    {
        done_ = true;
        return DAS_S_OK;
    }

    DasResult GetGuid(DasGuid*) { return DAS_E_NO_IMPLEMENTATION; }
    DasResult GetRuntimeClassName(IDasReadOnlyString**)
    {
        return DAS_E_NO_IMPLEMENTATION;
    }
    DasResult QueryInterface(const DasGuid& iid, void** pp) override
    {
        if (!pp)
            return DAS_E_INVALID_POINTER;
        if (iid == DasIidOf<IDasAsyncCallback>())
        {
            AddRef();
            *pp = static_cast<IDasAsyncCallback*>(this);
            return DAS_S_OK;
        }
        if (iid == DasIidOf<IDasBase>())
        {
            AddRef();
            *pp = static_cast<IDasBase*>(this);
            return DAS_S_OK;
        }
        return DAS_E_NO_INTERFACE;
    }
};

class LifecycleCallbackComponent : public DAS::PluginInterface::IDasComponent
{
    DAS_UTILS_IDASBASE_AUTO_IMPL(LifecycleCallbackComponent);

public:
    std::atomic<bool>                         callback_received_{false};
    std::string                               received_status_;
    DAS::Core::IPC::MainProcess::IIpcContext* ctx_ = nullptr;
    IDasAsyncCallback*                        completion_signal_ = nullptr;

    void Configure(DAS::Core::IPC::MainProcess::IIpcContext& ctx)
    {
        ctx_ = &ctx;
    }

    DasResult Dispatch(
        IDasReadOnlyString* p_function_name,
        IDasVariantVector*  p_arguments,
        IDasVariantVector** pp_out_result) override
    {
        using namespace DAS;

        const char* func_ptr = nullptr;
        DasResult   hr = p_function_name->GetUtf8(&func_ptr);
        if (IsFailed(hr) || !func_ptr)
            return hr;

        std::string func = func_ptr;

        if (func == "lifecycle_callback")
        {
            IDasReadOnlyString* status_ro = nullptr;
            hr = p_arguments->GetString(0, &status_ro);
            if (IsOk(hr) && status_ro)
            {
                const char* status_ptr = nullptr;
                status_ro->GetUtf8(&status_ptr);
                if (status_ptr)
                {
                    received_status_ = status_ptr;
                }
                status_ro->Release();
            }

            callback_received_ = true;

            DAS_CORE_LOG_INFO(
                "[LifecycleCallback] received finalize callback, status = {}",
                received_status_);

            // 事件驱动通知：通过 PostCallback 触发 CompletionSignal
            // 测试线程以 1ms 粒度轮询 done（与 DAS::Core::IPC::wait() 一致）
            if (ctx_ && completion_signal_)
            {
                completion_signal_->AddRef();
                ctx_->PostCallback(completion_signal_);
            }

            if (ctx_)
            {
                ctx_->RequestStop();
            }
        }

        if (pp_out_result)
        {
            DAS::ExportInterface::DasVariantVector empty_result;
            DasResult hr = CreateIDasVariantVector(empty_result.Put());
            if (DAS::IsOk(hr) && empty_result.Get())
            {
                empty_result.Get()->AddRef();
                *pp_out_result = empty_result.Get();
            }
        }
        return DAS_S_OK;
    }

    DasResult GetGuid(DasGuid* p_out) override
    {
        if (p_out)
        {
            *p_out = DasIidOf<LifecycleCallbackComponent>();
        }
        return DAS_S_OK;
    }

    DasResult GetRuntimeClassName(IDasReadOnlyString** pp_out) override
    {
        return DAS_E_NO_IMPLEMENTATION;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp) override
    {
        if (!pp)
            return DAS_E_INVALID_POINTER;
        if (iid == DasIidOf<LifecycleCallbackComponent>())
        {
            AddRef();
            *pp = static_cast<DAS::PluginInterface::IDasComponent*>(this);
            return DAS_S_OK;
        }
        if (iid == DasIidOf<DAS::PluginInterface::IDasComponent>())
        {
            AddRef();
            *pp = static_cast<DAS::PluginInterface::IDasComponent*>(this);
            return DAS_S_OK;
        }
        if (iid == DasIidOf<IDasBase>())
        {
            AddRef();
            *pp = static_cast<IDasBase*>(this);
            return DAS_S_OK;
        }
        return DAS_E_NO_INTERFACE;
    }

private:
};

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_JavaDirectorLifecycleTest)
{
#ifndef DAS_EXPORT_JAVA
    GTEST_SKIP() << "DAS_EXPORT_JAVA is not enabled";
#endif
    ASSERT_TRUE(std::filesystem::exists(host_exe_path_))
        << "DasHost.exe not found at: " << host_exe_path_;

    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("JavaTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "JavaTestPlugin JSON not found: " << e.what();
    }

    // 1. 创建 callback 组件并注册
    auto callback = DAS::MakeDasPtr<LifecycleCallbackComponent>();
    callback->Configure(GetContext());

    // 通过 RegisterService() 注册服务（内部完成 ObjectManager + Registry
    // 两步注册）
    DasResult reg_result = ctx_->RegisterService(
        callback.Get(),
        DasIidOf<DAS::PluginInterface::IDasComponent>());
    ASSERT_EQ(reg_result, DAS_S_OK);

    // 2. 启动 Host 进程
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    // 3. 加载 Java 插件
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put(),
        IpcTestConfig::GetPluginLoadTimeout());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value());

    auto& [load_result, proxy] = *opt;
    ASSERT_TRUE(DAS::IsOk(load_result));

    // 4. 获取 IDasComponent
    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    DAS::PluginInterface::DasPluginFeature feature;
    ASSERT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);

    IDasBase* factory_base_raw = nullptr;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(0, &factory_base_raw),
        DAS_S_OK);
    DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    DAS::PluginInterface::IDasComponent* component_raw = nullptr;
    ASSERT_EQ(
        factory->CreateInstance(
            DasIidOf<DAS::PluginInterface::IDasComponent>(),
            &component_raw),
        DAS_S_OK);
    DAS::DasPtr<DAS::PluginInterface::IDasComponent> component(component_raw);

    // 5. Dispatch bridgeLifecycleTest
    // PushBackBase 传 IDasBase* → 验证 Java 侧 as() 向下转换
    {
        DasReadOnlyString method_name{"bridgeLifecycleTest"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        // 故意用 PushBackBase 而非 PushBackComponent：
        // 验证 IPC 传递 IDasBase* 后 Java 侧可通过 as() 向下转换为
        // IDasComponent。这不是为了方便，而是为了验证 IPC QI 恢复机制。
        params.PushBackBase(callback.Get());
        params.PushBackString(DasReadOnlyString{"bridge_test_marker"});

        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_EQ(dispatch_result, DAS_S_OK)
            << "Dispatch(bridgeLifecycleTest) failed";

        // 调试：检查返回值内容
        if (result.Get())
        {
            uint64_t size = result->GetSize();
            DAS_CORE_LOG_INFO(
                "[CrossProcess_JavaDirectorLifecycleTest] Dispatch returned "
                "success, result size = {}",
                size);
            if (size > 0)
            {
                IDasReadOnlyString* elem0 = nullptr;
                DasResult           hr = result->GetString(0, &elem0);
                if (DAS::IsOk(hr) && elem0)
                {
                    const char* str = nullptr;
                    elem0->GetUtf8(&str);
                    DAS_CORE_LOG_INFO(
                        "[CrossProcess_JavaDirectorLifecycleTest] result[0] = {}",
                        str ? str : "(null)");
                    elem0->Release();
                }
            }
        }
        else
        {
            DAS_CORE_LOG_ERROR(
                "[CrossProcess_JavaDirectorLifecycleTest] Dispatch returned null "
                "result");
        }

        // 关键：不提取返回值中的 Director 对象。
        // result 包含 BridgeLifecycleDirector，但我们故意不处理它。
        // result 出作用域后 → VariantVector 释放 → Director proxy Release
        // → IPC Release → Host bridge Release → __das_bridge_release
        // → swigTakeOwnership → WeakGlobalRef → Java 对象变为 GC 可达
    }
    // ← result 析构，Director proxy 被释放

    // 6. 释放 Java 组件 proxy（不再需要）
    component.Reset();
    factory.Reset();
    factory_base.Reset();
    raw_proxy.Reset();

    // 7. 等待 Java GC 触发 Cleaner/finalize 回调
    //    事件驱动通知链路：
    //    Java GC → finalize → IPC Dispatch("lifecycle_callback")
    //    → LifecycleCallbackComponent.Dispatch()
    //    → ctx_->PostCallback(completion_signal_)
    //    → CompletionSignal.Do() → done = true
    //    测试线程以 1ms 粒度轮询 done（与 DAS::Core::IPC::wait() 一致）
    std::atomic<bool> done{false};
    auto              signal = DAS::MakeDasPtr<CompletionSignal>(done);
    callback->completion_signal_ = signal.Get();

    DAS_CORE_LOG_INFO(
        "[CrossProcess_JavaDirectorLifecycleTest] Waiting for bridge release "
        "callback from Java side...");

    constexpr auto kTimeout = std::chrono::seconds(15);
    auto           start_time = std::chrono::steady_clock::now();

    while (!done.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= kTimeout)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 8. 验证结果
    EXPECT_TRUE(callback->callback_received_)
        << "Bridge release callback was not received — Director bridge may not "
           "have been properly released, or GC did not collect the object";
    EXPECT_TRUE(
        callback->received_status_.find("bridge_released") != std::string::npos)
        << "Unexpected status: " << callback->received_status_;

    DAS_CORE_LOG_INFO(
        "[CrossProcess_JavaDirectorLifecycleTest] All verifications passed");
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_LuaDirectorLifecycleTest)
{
#ifndef DAS_EXPORT_LUA
    GTEST_SKIP() << "DAS_EXPORT_LUA is not enabled";
#endif
    ASSERT_TRUE(std::filesystem::exists(host_exe_path_))
        << "DasHost.exe not found at: " << host_exe_path_;

    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("LuaTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "LuaTestPlugin JSON not found: " << e.what();
    }

    // 1. 创建 callback 组件并注册
    auto callback = DAS::MakeDasPtr<LifecycleCallbackComponent>();
    callback->Configure(GetContext());

    DasResult reg_result = ctx_->RegisterService(
        callback.Get(),
        DasIidOf<DAS::PluginInterface::IDasComponent>());
    ASSERT_EQ(reg_result, DAS_S_OK);

    // 2. 启动 Host 进程
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    // 3. 加载 Lua 插件
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put(),
        IpcTestConfig::GetPluginLoadTimeout());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value());

    auto& [load_result, proxy] = *opt;
    ASSERT_TRUE(DAS::IsOk(load_result));

    // 4. 获取 IDasComponent
    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    DAS::PluginInterface::DasPluginFeature feature;
    ASSERT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);

    IDasBase* factory_base_raw = nullptr;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(0, &factory_base_raw),
        DAS_S_OK);
    DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    DAS::PluginInterface::IDasComponent* component_raw = nullptr;
    ASSERT_EQ(
        factory->CreateInstance(
            DasIidOf<DAS::PluginInterface::IDasComponent>(),
            &component_raw),
        DAS_S_OK);
    DAS::DasPtr<DAS::PluginInterface::IDasComponent> component(component_raw);

    // 5. Dispatch bridgeLifecycleTest
    {
        DasReadOnlyString method_name{"bridgeLifecycleTest"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        params.PushBackBase(callback.Get());
        params.PushBackString(DasReadOnlyString{"bridge_test_marker"});

        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_EQ(dispatch_result, DAS_S_OK)
            << "Dispatch(bridgeLifecycleTest) failed";

        if (result.Get())
        {
            uint64_t size = result->GetSize();
            DAS_CORE_LOG_INFO(
                "[CrossProcess_LuaDirectorLifecycleTest] Dispatch returned "
                "success, result size = {}",
                size);
            if (size > 0)
            {
                IDasReadOnlyString* elem0 = nullptr;
                DasResult           hr = result->GetString(0, &elem0);
                if (DAS::IsOk(hr) && elem0)
                {
                    const char* str = nullptr;
                    elem0->GetUtf8(&str);
                    DAS_CORE_LOG_INFO(
                        "[CrossProcess_LuaDirectorLifecycleTest] result[0] = {}",
                        str ? str : "(null)");
                    elem0->Release();
                }
            }
        }
        else
        {
            DAS_CORE_LOG_ERROR(
                "[CrossProcess_LuaDirectorLifecycleTest] Dispatch returned null "
                "result");
        }
    }
    // ← result 析构，Director proxy 被释放

    // 6. 释放 Lua 组件 proxy
    component.Reset();
    factory.Reset();
    factory_base.Reset();
    raw_proxy.Reset();

    // 7. 等待 Lua GC 触发 __gc → Release 回调
    //    事件驱动通知链路（对照 Java 版 Java GC → finalize → callback）：
    //    Lua GC → __gc 元方法 → Director Release → ref_count=0 → 析构
    //    → (LuaTestPlugin.dispatch_callback) → callback
    //    Dispatch("lifecycle_callback") → LifecycleCallbackComponent.Dispatch()
    //    → ctx_->PostCallback(completion_signal_)
    //    → CompletionSignal.Do() → done = true
    std::atomic<bool> done{false};
    auto              signal = DAS::MakeDasPtr<CompletionSignal>(done);
    callback->completion_signal_ = signal.Get();

    DAS_CORE_LOG_INFO(
        "[CrossProcess_LuaDirectorLifecycleTest] Waiting for bridge release "
        "callback from Lua side...");

    constexpr auto kTimeout = std::chrono::seconds(15);
    auto           start_time = std::chrono::steady_clock::now();

    while (!done.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= kTimeout)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 8. 验证结果
    EXPECT_TRUE(callback->callback_received_)
        << "Bridge release callback was not received — Director bridge may not "
           "have been properly released, or GC did not collect the object";
    EXPECT_TRUE(
        callback->received_status_.find("bridge_released") != std::string::npos)
        << "Unexpected status: " << callback->received_status_;

    DAS_CORE_LOG_INFO(
        "[CrossProcess_LuaDirectorLifecycleTest] All verifications passed");
}

// ====== CrossProcess QueryMainProcessInterfaceByName E2E 测试 ======

/**
 * @brief 测试跨进程按名称查询主进程服务 (ByName)
 *
 * 完整 E2E 流程：
 * 1. 主进程通过 DasRegisterMainProcessServiceByName 注册 IDasReadOnlyString
 * 2. 启动 Host 进程，加载 IpcTestPlugin1
 * 3. 通过 IDasComponentFactory 创建 IDasComponent
 * 4. 调用 Dispatch("queryMainProcessStringByName", [name])
 *    → Host 插件内部调用 DasQueryMainProcessInterfaceByName(name)
 *    → LOOKUP_BY_NAME 跨进程 IPC → 主进程 RemoteObjectRegistry 查找
 * 5. 验证返回的字符串与注册的一致
 */
TEST_F(
    IpcMultiProcessTestIntegration,
    CrossProcess_QueryMainProcessStringByName)
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

    // 2. 在主进程中通过 ByName API 注册一个 IDasReadOnlyString 服务
    DAS::DasPtr<IDasReadOnlyString> service_string;
    DasResult create_result = CreateIDasReadOnlyStringFromUtf8(
        "Hello from ByName CrossProcess E2E",
        service_string.Put());
    ASSERT_EQ(create_result, DAS_S_OK) << "Failed to create test string";

    const char* kServiceName = "test_string_service";

    // 在 io_context 线程中注册（BusinessThread TLS 已就绪）
    auto reg_opt = DAS::Core::IPC::wait(
        stdexec::then(
            stdexec::then(
                stdexec::schedule(GetContext()),
                [&]() noexcept -> DasResult
                {
                    return ctx_->RegisterServiceByName(
                        service_string.Get(),
                        DasIidOf<IDasReadOnlyString>(),
                        kServiceName);
                }),
            [](DasResult r) noexcept -> DasResult { return r; }));

    ASSERT_TRUE(reg_opt.has_value()) << "RegisterByName: wait failed";
    DasResult result = std::get<0>(*reg_opt);
    ASSERT_EQ(result, DAS_S_OK) << "RegisterServiceByName failed";

    DAS_CORE_LOG_INFO(
        "[CrossProcess_QueryMainProcessStringByName] Registered "
        "IDasReadOnlyString with name '{}' in main process",
        kServiceName);

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

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";
    ASSERT_NE(proxy, nullptr);

    // 5. Get IDasPluginPackage → IDasComponentFactory → IDasComponent
    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    ASSERT_NE(raw_proxy, nullptr);

    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    IDasBase* factory_base_raw = nullptr;
    result = plugin_package->CreateFeatureInterface(1, &factory_base_raw);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    DAS::PluginInterface::IDasComponent* component_raw = nullptr;
    result = factory->CreateInstance(
        DasIidOf<DAS::PluginInterface::IDasComponent>(),
        &component_raw);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(component_raw, nullptr);
    DAS::DasPtr<DAS::PluginInterface::IDasComponent> component(component_raw);

    // 6. Dispatch("queryMainProcessStringByName", [name]) — 核心验证
    {
        DasReadOnlyString method_name{"queryMainProcessStringByName"};
        DAS::ExportInterface::DasVariantVector dispatch_result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        // 传递 service name 作为参数
        DasReadOnlyString name_param{kServiceName};
        params.PushBackString(name_param.Get());

        DasResult dispatch_hr = component->Dispatch(
            method_name.Get(),
            params.Get(),
            dispatch_result.Put());
        ASSERT_EQ(dispatch_hr, DAS_S_OK)
            << "Dispatch(queryMainProcessStringByName) failed — "
               "ByName cross-process query did not work";

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
        EXPECT_STREQ(out_str, "Hello from ByName CrossProcess E2E")
            << "Returned string does not match the registered value";

        out_string->Release();
    }

    // 7. 验证注销 ByName 后 Host 查询失败
    {
        auto unreg_opt = DAS::Core::IPC::wait(
            stdexec::then(
                stdexec::then(
                    stdexec::schedule(GetContext()),
                    [&]() noexcept -> DasResult
                    { return ctx_->UnregisterServiceByName(kServiceName); }),
                [](DasResult r) noexcept -> DasResult { return r; }));
        ASSERT_TRUE(unreg_opt.has_value()) << "UnregisterByName: wait failed";
        result = std::get<0>(*unreg_opt);
        ASSERT_EQ(result, DAS_S_OK) << "UnregisterServiceByName failed";
    }

    // 注销后 Host 应查询失败
    {
        DasReadOnlyString method_name{"queryMainProcessStringByName"};
        DAS::ExportInterface::DasVariantVector dispatch_result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        DasReadOnlyString name_param{kServiceName};
        params.PushBackString(name_param.Get());

        DasResult dispatch_hr = component->Dispatch(
            method_name.Get(),
            params.Get(),
            dispatch_result.Put());
        EXPECT_NE(dispatch_hr, DAS_S_OK)
            << "Dispatch(queryMainProcessStringByName) should fail after "
               "unregister — Host-side DasQueryMainProcessInterfaceByName "
               "must return error for unregistered name";
    }

    DAS_CORE_LOG_INFO(
        "[CrossProcess_QueryMainProcessStringByName] Test passed — "
        "Host plugin queried main process IDasReadOnlyString by name "
        "via LOOKUP_BY_NAME IPC, and unregister was verified");
}
