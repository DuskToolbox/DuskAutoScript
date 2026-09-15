// FormSequence launch 端到端测试：起源痛点场景（目标程序未启动 → 自动启动
// + 等窗口出现 + 执行被编排任务）的可重复自动化证明。
// 拓扑照真实部署形态：调度器在测试进程内，DasGraphTask 与 FlowControl 均
// 走 IPC 加载（各自独立 host 兄弟进程）——launch 组件被 GraphTask host 跨
// host 调用时，代理腿经 main 中转，跨 host 消息中转链路是本测试覆盖面的
// 一部分。
// 窗口替身 = StubWindowTarget.exe（隐藏创建 + 延迟显示，模拟「spawn 到窗口
// 出现有数秒延迟」）；launch 组件 spawn 替身并轮询可见窗口，窗口出现后发
// signal_out 门控后继探针组件节点执行（FakeTaskComponent 计数观测）。
#include <IpcTestConfig.h>
// winsock 依赖序：本 TU 的 Windows.h 先于 asio（经 PluginManager 头间接
// 进入），LEAN_AND_MEAN 让 Windows.h 跳过 WinSock v1，asio 随后自带
// winsock2 不再冲突。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/DasApi.h>
#include <das/DasRef.hpp>
#include <das/Plugins/SchedulerTestPlugin/SchedulerTestSharedState.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <das/DasConfig.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_INTERPROCESS_WARNING
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
DAS_DISABLE_WARNING_END

// clang-format off
// TestablePluginManager.h 必须在所有 STL/业务头之后 include：它内部
// #define private protected 让 PluginManager 私有字段可被子类访问，
// 若 STL 头未先 set include guard，MSVC xkeycheck.h 会拒绝 #define 关键字。
#include "TestablePluginManager.h"
// clang-format on

namespace
{
    // 既有注册面常量（DasGraphTask / DasFlowControl manifest 与记录型执行
    // 组件已在别处定义，非本测试新增）。
    constexpr std::string_view GRAPH_TASK_TYPE_GUID =
        "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
    constexpr std::string_view GRAPH_PLUGIN_GUID =
        "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
    constexpr std::string_view LAUNCH_COMPONENT_GUID =
        "95FD2FFA-88F2-4F85-A536-C708D25B641F";
    constexpr std::string_view FLOW_CONTROL_PLUGIN_GUID =
        "7115BBFA-7E57-494E-82D5-D4134C367E13";

    // formSequence itemId（GUID 形态沿用历史约定，任意唯一字符串均可）。
    constexpr std::string_view LAUNCH_ITEM_ID =
        "5A192404-4C64-4E4A-A6E8-7E7B2A6D1DCB";
    constexpr std::string_view PROBE_ITEM_ID =
        "625A1850-E1EA-4569-8FC3-A5D804E06535";

    constexpr int64_t STUB_WINDOW_DELAY_MS = 800;
    constexpr int64_t STUB_WINDOW_LIFETIME_MS = 60000;
    constexpr int64_t WAIT_WINDOW_TIMEOUT_MS = 20000;

    std::filesystem::path UniqueE2ETestDir()
    {
        static std::atomic<int> counter{0};
        const auto              name =
            "formseq_launch_e2e_" + std::to_string(counter.fetch_add(1));
        auto path =
            std::filesystem::current_path() / Das::Utils::PathFromUtf8(name);
        std::filesystem::remove_all(path);
        return path;
    }

    // 窗口替身 exe 定位：优先 ctest WORKING_DIRECTORY（= Test 输出目录），
    // 回退测试 exe 自身目录（替身与测试 exe 同目录产出）。
    std::filesystem::path ResolveStubWindowTargetPath()
    {
        namespace fs = std::filesystem;
        const fs::path from_working_dir =
            fs::current_path() / DAS_PATH_LITERAL("StubWindowTarget.exe");
        if (fs::exists(from_working_dir))
        {
            return from_working_dir;
        }
        wchar_t module_path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) > 0)
        {
            const fs::path fallback =
                fs::path(module_path).parent_path()
                / DAS_PATH_LITERAL("StubWindowTarget.exe");
            if (fs::exists(fallback))
            {
                return fallback;
            }
        }
        return from_working_dir;
    }

    // 每次运行唯一的窗口类名/标题 token：防同机历史残留替身窗口造成
    // ambiguous 匹配，也让标题正则不误伤无关窗口（regex_search 语义）。
    std::string MakeUniqueWindowToken()
    {
        static std::atomic<uint64_t> counter{0};
        return std::to_string(::GetCurrentProcessId()) + "_"
               + std::to_string(counter.fetch_add(1));
    }

    void WriteSchedulerInstanceState(
        Das::Core::SettingsManager::SettingsManager& settings_manager,
        const std::vector<yyjson::value>&            task_instances)
    {
        yyjson::value index(Das::Utils::MakeYyjsonObject());
        (*index.as_object())[std::string_view("nextTaskId")] =
            static_cast<int64_t>(task_instances.size());
        {
            yyjson::value order_array(Das::Utils::MakeYyjsonArray());
            for (size_t position = 0; position < task_instances.size();
                 ++position)
            {
                (*order_array.as_array())
                    .emplace_back(static_cast<int64_t>(position));
            }
            (*index.as_object())[std::string_view("taskOrder")] =
                std::move(order_array);
        }
        settings_manager.UpdateSchedulerIndexJson("0", index);

        for (size_t position = 0; position < task_instances.size(); ++position)
        {
            settings_manager.UpdateTaskInstanceJson(
                "0",
                static_cast<int64_t>(position),
                task_instances[position]);
        }
    }

    // 专用最小 manifest：假任务类型 + 严格 authoring 工厂 + 记录型执行组件
    // （FakeTaskComponent 自身 guid——流程组件 guid 属真实 FlowControl 插件，
    // 二者同场部署时以此避免 guid 冲突）。
    void WriteLaunchE2EFactoryManifest(
        const std::filesystem::path& manifest_path)
    {
        yyjson::value manifest(Das::Utils::MakeYyjsonObject());
        (*manifest.as_object())[std::string_view("name")] = "FactoryPlugin";
        (*manifest.as_object())[std::string_view("author")] = "Tests";
        (*manifest.as_object())[std::string_view("version")] = "1.0";
        (*manifest.as_object())[std::string_view("guid")] =
            std::string(FactoryPluginGuidString);
        (*manifest.as_object())[std::string_view("description")] =
            "Factory-backed scheduler test plugin";
        (*manifest.as_object())[std::string_view("supportedSystem")] =
            "Windows";
        (*manifest.as_object())[std::string_view("language")] = "Cpp";
        (*manifest.as_object())[std::string_view("loadMode")] = "ipc";
        (*manifest.as_object())[std::string_view("pluginFilenameExtension")] =
            "dll";
        (*manifest.as_object())[std::string_view("settings")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        {
            yyjson::value task_components(Das::Utils::MakeYyjsonObject());
            {
                yyjson::value factories(Das::Utils::MakeYyjsonArray());
                factories.as_array()->emplace_back(
                    FactoryTaskComponentFactoryGuidString);
                (*task_components.as_object())[std::string_view("factories")] =
                    std::move(factories);
            }
            {
                yyjson::value definition(Das::Utils::MakeYyjsonObject());
                (*definition.as_object())[std::string_view("schemaVersion")] =
                    static_cast<int64_t>(1);
                (*definition.as_object())[std::string_view("kind")] =
                    "test.execution";
                (*definition.as_object())[std::string_view("componentGuid")] =
                    std::string(FactoryTaskComponentImplGuidString);
                (*definition.as_object())[std::string_view("inputs")] =
                    yyjson::value(Das::Utils::MakeYyjsonArray());
                (*definition.as_object())[std::string_view("outputs")] =
                    yyjson::value(Das::Utils::MakeYyjsonArray());
                (*definition.as_object())[std::string_view("config")] =
                    yyjson::value(Das::Utils::MakeYyjsonObject());
                (*definition.as_object())[std::string_view("diagnostics")] =
                    yyjson::value(Das::Utils::MakeYyjsonArray());

                yyjson::value entry(Das::Utils::MakeYyjsonObject());
                (*entry.as_object())[std::string_view("factoryGuid")] =
                    FactoryTaskComponentFactoryGuidString;
                (*entry.as_object())[std::string_view("definition")] =
                    std::move(definition);

                yyjson::value components(Das::Utils::MakeYyjsonObject());
                (*components.as_object())[std::string(
                    FactoryTaskComponentImplGuidString)] = std::move(entry);
                (*task_components.as_object())[std::string_view("components")] =
                    std::move(components);
            }
            (*manifest.as_object())[std::string_view("taskComponents")] =
                std::move(task_components);
        }
        {
            yyjson::value task_entry(Das::Utils::MakeYyjsonObject());
            (*task_entry.as_object())[std::string_view("pluginGuid")] =
                std::string(FactoryPluginGuidString);
            (*task_entry.as_object())[std::string_view("name")] = "factoryTask";
            (*task_entry.as_object())[std::string_view("description")] =
                "Scheduler test task";
            {
                // descriptors 是任务类型 DTO 的必位键（caster 缺键即解析
                // 失败、任务类型静默不注册）。
                yyjson::value descriptors(Das::Utils::MakeYyjsonArray());
                {
                    yyjson::value descriptor(Das::Utils::MakeYyjsonObject());
                    (*descriptor.as_object())[std::string_view("name")] =
                        "key1";
                    (*descriptor.as_object())[std::string_view("type")] =
                        static_cast<int64_t>(
                            Das::ExportInterface::DAS_TYPE_STRING);
                    (*descriptor
                          .as_object())[std::string_view("defaultValue")] =
                        "default";
                    descriptors.as_array()->emplace_back(std::move(descriptor));
                }
                {
                    yyjson::value descriptor(Das::Utils::MakeYyjsonObject());
                    (*descriptor.as_object())[std::string_view("name")] =
                        "retryCount";
                    (*descriptor.as_object())[std::string_view("type")] =
                        static_cast<int64_t>(
                            Das::ExportInterface::DAS_TYPE_INT);
                    (*descriptor
                          .as_object())[std::string_view("defaultValue")] =
                        static_cast<int64_t>(3);
                    descriptors.as_array()->emplace_back(std::move(descriptor));
                }
                (*task_entry.as_object())[std::string_view("descriptors")] =
                    std::move(descriptors);
            }
            {
                yyjson::value authoring(Das::Utils::MakeYyjsonObject());
                (*authoring.as_object())[std::string_view("factoryGuid")] =
                    FactoryAuthoringFactoryGuidString;
                yyjson::value supported(Das::Utils::MakeYyjsonArray());
                supported.as_array()->emplace_back("formSequence");
                (*authoring.as_object())[std::string_view("supportedKinds")] =
                    std::move(supported);
                (*task_entry.as_object())[std::string_view("authoring")] =
                    std::move(authoring);
            }
            {
                yyjson::value execution_component(
                    Das::Utils::MakeYyjsonObject());
                (*execution_component
                      .as_object())[std::string_view("componentGuid")] =
                    std::string(FactoryTaskComponentImplGuidString);
                (*task_entry
                      .as_object())[std::string_view("executionComponent")] =
                    std::move(execution_component);
            }
            yyjson::value tasks_object(Das::Utils::MakeYyjsonObject());
            (*tasks_object.as_object())[std::string(FactoryTaskGuidString)] =
                std::move(task_entry);
            (*manifest.as_object())[std::string_view("tasks")] =
                std::move(tasks_object);
        }

        std::ofstream output_stream(manifest_path);
        output_stream << *Das::Utils::SerializeYyjsonValue(manifest, true);
    }

    class FormSequenceLaunchE2ETest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            test_dir_ = UniqueE2ETestDir();
            std::filesystem::create_directories(test_dir_);
            settings_dir_ = test_dir_ / DAS_PATH_LITERAL("settings");
            std::filesystem::create_directories(settings_dir_);
            plugin_dir_ = test_dir_ / DAS_PATH_LITERAL("plugins");
            std::filesystem::create_directories(plugin_dir_);
            manifest_path_ =
                plugin_dir_ / DAS_PATH_LITERAL("FactoryPlugin.json");
            WriteLaunchE2EFactoryManifest(manifest_path_);

            // 先建共享内存段并构造 FactoryTaskSharedState，再经环境变量
            // 告知 DasHost 子进程内的插件 dll 打开同一段。
            CreateSharedStateSegment();
            DeploySchedulerTestPlugin();

            settings_manager_ =
                std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                    settings_dir_);
            settings_manager_->CreateProfile("0");

            ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContext(false);
            plugin_manager_ = std::make_unique<
                Das::Core::ForeignInterfaceHost::TestablePluginManager>(
                *settings_manager_,
                Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                    ipc_sp_.Get()));
            try
            {
                const auto host_path = IpcTestConfig::GetDasHostPath();
                if (std::filesystem::exists(host_path))
                {
                    // ABI 边界收 const std::string&，path 在调用点物化为 UTF-8
                    plugin_manager_->SetHostExePath(
                        Das::Utils::PathToUtf8(host_path));
                }
            }
            catch (const std::exception&)
            {
                // host 未配置时 PluginManager.Initialize 会失败，测试断言会
                // 暴露具体错误，这里不提前 fail。
            }

            scheduler_ =
                std::make_unique<Das::Core::TaskScheduler::SchedulerService>(
                    *plugin_manager_,
                    Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                        ipc_sp_.Get()));

            ASSERT_EQ(plugin_manager_->Initialize(1), DAS_S_OK);

            // IpcContext::Run() 置 running_ 后阻塞在 io_context->run()，
            // 接收协程依赖 running_ 为真才能持续泵入站消息。
            io_thread_ = std::thread([this]() { ipc_sp_->Run(); });
        }

        void CreateSharedStateSegment()
        {
            const uint64_t pid = static_cast<uint64_t>(::GetCurrentProcessId());
            static std::atomic<uint64_t> segment_counter{0};
            shm_name_ = "DasFormSeqLaunchE2E_" + std::to_string(pid) + "_"
                        + std::to_string(segment_counter.fetch_add(1));

            boost::interprocess::shared_memory_object::remove(
                shm_name_.c_str());

            const size_t segment_size = sizeof(FactoryTaskSharedState) + 4096;
            shm_segment_ =
                std::make_unique<boost::interprocess::managed_shared_memory>(
                    boost::interprocess::create_only_t{},
                    shm_name_.c_str(),
                    segment_size);

            shared_state_ =
                shm_segment_->construct<FactoryTaskSharedState>("state")();

            _putenv_s("DAS_SCHEDULER_TEST_SHM_NAME", shm_name_.c_str());
        }

        // 部署真实 SchedulerTestPlugin 产物（以 FactoryPlugin.dll 之名）。
        void DeploySchedulerTestPlugin()
        {
            namespace fs = std::filesystem;
            const fs::path build_plugin_dir = IpcTestConfig::GetPluginDir();
#ifndef DAS_SCHEDULER_TEST_PLUGIN_FILENAME
#error "DAS_SCHEDULER_TEST_PLUGIN_FILENAME must be defined via CMake"
#endif
            const fs::path source_dll =
                build_plugin_dir
                / DAS_PATH_LITERAL(DAS_SCHEDULER_TEST_PLUGIN_FILENAME);
            ASSERT_TRUE(fs::exists(source_dll))
                << "SchedulerTestPlugin artifact not found at: "
                << Das::Utils::PathToUtf8(source_dll);

            const fs::path target_dll =
                plugin_dir_ / DAS_PATH_LITERAL("FactoryPlugin.dll");
            fs::copy_file(
                source_dll,
                target_dll,
                fs::copy_options::overwrite_existing);
        }

        // 部署构建产物真实插件（dll + json 成对拷贝；force_ipc 时补写
        // loadMode=ipc——测试 exe 静态链接 DasCore，进程内再加载插件 dll
        // 会产生双份 DasCore 全局状态）。
        void DeployPluginArtifacts(
            const std::string& dll_name,
            const std::string& manifest_name,
            bool               force_ipc)
        {
            namespace fs = std::filesystem;
            const fs::path build_plugin_dir = IpcTestConfig::GetPluginDir();

            const fs::path source_dll =
                build_plugin_dir / Das::Utils::PathFromUtf8(dll_name);
            ASSERT_TRUE(fs::exists(source_dll))
                << "plugin dll not found at: "
                << Das::Utils::PathToUtf8(source_dll);
            fs::copy_file(
                source_dll,
                plugin_dir_ / Das::Utils::PathFromUtf8(dll_name),
                fs::copy_options::overwrite_existing);

            const fs::path source_manifest =
                build_plugin_dir / Das::Utils::PathFromUtf8(manifest_name);
            ASSERT_TRUE(fs::exists(source_manifest))
                << "plugin manifest not found at: "
                << Das::Utils::PathToUtf8(source_manifest);
            if (!force_ipc)
            {
                fs::copy_file(
                    source_manifest,
                    plugin_dir_ / Das::Utils::PathFromUtf8(manifest_name),
                    fs::copy_options::overwrite_existing);
                return;
            }

            std::ifstream     input_stream(source_manifest);
            const std::string manifest_text(
                (std::istreambuf_iterator<char>(input_stream)),
                std::istreambuf_iterator<char>());
            auto parsed = Das::Utils::ParseYyjsonFromString(manifest_text);
            ASSERT_TRUE(parsed.has_value());
            auto parsed_object = parsed->as_object();
            ASSERT_TRUE(parsed_object.has_value());
            (*parsed_object)[std::string_view("loadMode")] = "ipc";
            std::ofstream output_stream(
                plugin_dir_ / Das::Utils::PathFromUtf8(manifest_name),
                std::ios::trunc);
            output_stream << Das::Utils::SerializeYyjsonValue(*parsed).value();
        }

        void TearDown() override
        {
            // SetUp 早期步骤可能抛异常退出，成员处于半初始化状态，逐个
            // 判空防解引用 nullptr。
            if (scheduler_
                && scheduler_->Status()
                       == Das::Core::TaskScheduler::SchedulerState::Running)
            {
                EXPECT_EQ(scheduler_->Disable(), DAS_S_OK);
            }

            if (ipc_sp_)
            {
                ipc_sp_->RequestStop();
            }
            if (io_thread_.joinable())
            {
                io_thread_.join();
            }

            scheduler_.reset();
            if (plugin_manager_)
            {
                plugin_manager_->Shutdown();
            }
            plugin_manager_.reset();
            settings_manager_.reset();
            if (shm_segment_)
            {
                shm_segment_->destroy<FactoryTaskSharedState>("state");
                shm_segment_.reset();
            }
            if (!shm_name_.empty())
            {
                boost::interprocess::shared_memory_object::remove(
                    shm_name_.c_str());
            }
            // test_dir_ 内含插件 dll，可能仍被映射，用不抛异常的 remove_all
            // 重载；文件残留不影响断言（下一测试用新的 UniqueE2ETestDir）。
            std::error_code error_code;
            std::filesystem::remove_all(test_dir_, error_code);
        }

        std::filesystem::path test_dir_;
        std::filesystem::path settings_dir_;
        std::filesystem::path plugin_dir_;
        std::filesystem::path manifest_path_;
        std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
                                                              settings_manager_;
        DAS::DasPtr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
        std::unique_ptr<Das::Core::ForeignInterfaceHost::TestablePluginManager>
            plugin_manager_;
        std::unique_ptr<Das::Core::TaskScheduler::SchedulerService> scheduler_;
        FactoryTaskSharedState* shared_state_ = nullptr;
        std::unique_ptr<boost::interprocess::managed_shared_memory>
                    shm_segment_;
        std::string shm_name_;
        std::thread io_thread_;
    };

    // 目标程序未启动等价场景：formSequence 图 = [launch 节点（spawn 窗口
    // 替身 + 等可见窗口出现）→ 探针组件节点（信号门控后执行并计数）]，
    // 经真实调度器 tick 端到端执行。
    TEST_F(
        FormSequenceLaunchE2ETest,
        LaunchWaitsForStubWindowThenSignalGatesProbeComponent)
    {
        DeployPluginArtifacts(
            "libDasGraphTask.dll",
            "DasGraphTask.json",
            /*force_ipc=*/true);
        DeployPluginArtifacts(
            "libDasFlowControl.dll",
            "DasFlowControl.json",
            /*force_ipc=*/true);

        const std::filesystem::path stub_exe = ResolveStubWindowTargetPath();
        ASSERT_TRUE(std::filesystem::exists(stub_exe))
            << "StubWindowTarget.exe not found at: "
            << Das::Utils::PathToUtf8(stub_exe);

        const std::string  window_token = MakeUniqueWindowToken();
        const std::string  window_class = "DasE2ELaunchStub_" + window_token;
        const std::string  window_title = "das-e2e-launch-" + window_token;
        const std::wstring window_class_wide(
            window_class.begin(),
            window_class.end());

        // graph 任务实例：formSequence 两项——launch 首节点 spawn 替身并等
        // 窗口（延迟 800ms 出现，超时 20s），探针次节点被 launch 的
        // signal_out 门控（信号边由投影自动生成，门控链路推进）。
        {
            yyjson::value task0(Das::Utils::MakeYyjsonObject());
            (*task0.as_object())[std::string_view("id")] =
                static_cast<int64_t>(0);
            (*task0.as_object())[std::string_view("taskGuid")] =
                std::string(GRAPH_TASK_TYPE_GUID);
            (*task0.as_object())[std::string_view("pluginGuid")] =
                std::string(GRAPH_PLUGIN_GUID);
            // 立即到期：null 会被排到默认长延迟，拖垮 tick 启动。
            (*task0.as_object())[std::string_view("nextExecutionTime")] =
                static_cast<int64_t>(1);
            {
                yyjson::value items(Das::Utils::MakeYyjsonArray());
                {
                    yyjson::value launch_item(Das::Utils::MakeYyjsonObject());
                    (*launch_item.as_object())[std::string_view("itemId")] =
                        std::string(LAUNCH_ITEM_ID);
                    {
                        yyjson::value target(Das::Utils::MakeYyjsonObject());
                        (*target.as_object())[std::string_view("targetKind")] =
                            "componentRef";
                        yyjson::value component_ref(
                            Das::Utils::MakeYyjsonObject());
                        (*component_ref.as_object())[std::string_view("kind")] =
                            "componentRef";
                        (*component_ref
                              .as_object())[std::string_view("componentGuid")] =
                            std::string(LAUNCH_COMPONENT_GUID);
                        (*component_ref
                              .as_object())[std::string_view("pluginGuid")] =
                            std::string(FLOW_CONTROL_PLUGIN_GUID);
                        (*target
                              .as_object())[std::string_view("componentRef")] =
                            std::move(component_ref);
                        (*launch_item.as_object())[std::string_view("target")] =
                            std::move(target);
                    }
                    {
                        yyjson::value settings(Das::Utils::MakeYyjsonObject());
                        const std::string stub_exe_path =
                            Das::Utils::PathToGenericUtf8(stub_exe);
                        (*settings.as_object())[std::string_view("exePath")] =
                            std::make_pair(
                                std::string_view(stub_exe_path),
                                yyjson::copy_string);
                        {
                            yyjson::value arguments(
                                Das::Utils::MakeYyjsonArray());
                            const std::array<std::string, 8> argument_values = {
                                "--class",
                                window_class,
                                "--title",
                                window_title,
                                "--delay-ms",
                                std::to_string(STUB_WINDOW_DELAY_MS),
                                "--lifetime-ms",
                                std::to_string(STUB_WINDOW_LIFETIME_MS)};
                            for (const auto& argument : argument_values)
                            {
                                // yyjson 对 lvalue std::string 是别名而非
                                // 拷贝（源离作用域即悬垂），字符串一律带
                                // copy_string 标签深拷贝入文档。
                                (*arguments.as_array())
                                    .emplace_back(
                                        argument,
                                        yyjson::copy_string);
                            }
                            (*settings.as_object())[std::string_view("args")] =
                                std::move(arguments);
                        }
                        {
                            // 等窗三键为顶层 config 字段；字符串一律带
                            // copy_string 标签深拷贝入文档（同上 args）。
                            (*settings.as_object())[std::string_view(
                                "waitWindowClassRegex")] =
                                std::make_pair(
                                    std::string_view(window_class),
                                    yyjson::copy_string);
                            (*settings.as_object())[std::string_view(
                                "waitWindowWindowRegex")] =
                                std::make_pair(
                                    std::string_view(window_title),
                                    yyjson::copy_string);
                            (*settings.as_object())[std::string_view(
                                "waitWindowTimeoutMs")] =
                                WAIT_WINDOW_TIMEOUT_MS;
                        }
                        (*launch_item
                              .as_object())[std::string_view("settings")] =
                            std::move(settings);
                    }
                    (*items.as_array()).emplace_back(std::move(launch_item));
                }
                {
                    yyjson::value probe_item(Das::Utils::MakeYyjsonObject());
                    (*probe_item.as_object())[std::string_view("itemId")] =
                        std::string(PROBE_ITEM_ID);
                    {
                        yyjson::value target(Das::Utils::MakeYyjsonObject());
                        (*target.as_object())[std::string_view("targetKind")] =
                            "componentRef";
                        yyjson::value component_ref(
                            Das::Utils::MakeYyjsonObject());
                        (*component_ref.as_object())[std::string_view("kind")] =
                            "componentRef";
                        (*component_ref
                              .as_object())[std::string_view("componentGuid")] =
                            std::string(FactoryTaskComponentImplGuidString);
                        (*component_ref
                              .as_object())[std::string_view("pluginGuid")] =
                            std::string(FactoryPluginGuidString);
                        (*target
                              .as_object())[std::string_view("componentRef")] =
                            std::move(component_ref);
                        (*probe_item.as_object())[std::string_view("target")] =
                            std::move(target);
                    }
                    (*probe_item.as_object())[std::string_view("settings")] =
                        yyjson::value(Das::Utils::MakeYyjsonObject());
                    (*items.as_array()).emplace_back(std::move(probe_item));
                }
                yyjson::value properties(Das::Utils::MakeYyjsonObject());
                (*properties.as_object())[std::string_view("items")] =
                    std::move(items);
                (*task0.as_object())[std::string_view("properties")] =
                    std::move(properties);
            }
            WriteSchedulerInstanceState(*settings_manager_, {task0});
        }

        ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

        const auto tick_start = std::chrono::steady_clock::now();
        ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

        // 等链路推进到探针组件：launch 完成等窗 + 发 signal_out 门控后，
        // 探针节点执行并计数。真 spawn + 延迟出现 + 轮询 + IPC，余量给足。
        bool probe_executed = false;
        {
            ipc_scoped_lock lock(shared_state_->mutex);
            probe_executed = shared_state_->cv.wait_for(
                lock,
                std::chrono::seconds(60),
                [this] { return shared_state_->component_do_count >= 1; });
        }

        // 失败与否都先清窗口：WM_CLOSE 到唯一类名窗口；lifetime-ms 定时器
        // 是兜底（失败路径不留僵尸窗口）。
        const HWND stub_window =
            FindWindowW(window_class_wide.c_str(), nullptr);
        if (stub_window != nullptr)
        {
            PostMessageW(stub_window, WM_CLOSE, 0, 0);
        }

        ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
        // 让 BusinessThread 上的 OnTick 收尾，避免 TearDown 销毁 scheduler
        // 与 tick 尾段竞态。
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        ASSERT_TRUE(probe_executed)
            << "probe component never executed behind the launch signal gate";

        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tick_start)
                .count();
        // 等窗真实生效的行为证据：替身窗口延迟 800ms 才可见，launch 不等
        // 窗口就不可能完成，链路推进到探针组件的时刻必然晚于延迟。
        EXPECT_GE(elapsed_ms, STUB_WINDOW_DELAY_MS);

        // 替身窗口曾被创建且此刻仍在（等窗匹配成功后无人销毁它，仅测试
        // 清理与 lifetime 兜底会关）——launch-window-timeout 时窗口同样不会
        // 出现且链路不会推进，上面的断言已反证；此处直接取窗口存在证据。
        EXPECT_NE(stub_window, nullptr)
            << "stub window was never created or already vanished";

        const ipc_scoped_lock lock(shared_state_->mutex);
        // 探针恰好执行一次（信号门控链只推进一轮），直跑任务本体
        // （FactoryBackedTask）不应被执行——执行的是 graph 任务。
        EXPECT_EQ(shared_state_->component_do_count, 1u);
        EXPECT_EQ(shared_state_->executed_count, 0u);
    }
} // namespace
