// Phase 87/01 — authoring session cache (tracer: Get path).
//
// Verifies D-87-06 (cache hit) and D-87-09 (lazy create) via the
// FakeAuthoringSession shared-memory counter `authoring_session_count`:
//   * REQ-06a CacheHit        — two Get calls reuse one session (count stays 1)
//   * REQ-06b LazyCreate      — count is 0 before the first Get, 1 after
//
// The fixture mirrors SchedulerRuntimeBackedTest (SchedulerServiceTest.cpp):
// full IPC + managed-shared-memory setup so the real SchedulerTestPlugin is
// loaded in the DasHost subprocess and its FakeAuthoringSession mutates the
// same FactoryTaskSharedState this executable observes.

// boost::asio (pulled in transitively by SchedulerService.h) requires that
// winsock2.h be visible without the WinSock 1.x header; defining this before
// any include keeps <Windows.h> from pulling in winsock.h. SchedulerServiceTest
// achieves the same effect via its beast/controller headers.
#define WIN32_LEAN_AND_MEAN

#include <IpcTestConfig.h>
#include <Windows.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasApi.h>
#include <das/DasSharedRef.hpp>
#include <das/DasString.hpp>
#include <das/IDasSchedulerService.h>
#include <das/Plugins/SchedulerTestPlugin/SchedulerTestSharedState.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <das/Utils/DasJsonCore.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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

using namespace Das::Core::TaskScheduler;
using Das::DasOutPtr;
using Das::DasPtr;

namespace
{
    std::filesystem::path UniqueTestDir()
    {
        static std::atomic<int> counter{0};
        auto                    name = "scheduler_authoring_cache_test_"
                                       + std::to_string(counter.fetch_add(1));
        auto                    path = std::filesystem::current_path() / name;
        std::filesystem::remove_all(path);
        return path;
    }

    /// Helper to write scheduler index + task instance files directly.
    void WriteSchedulerState(
        Das::Core::SettingsManager::SettingsManager& sm,
        int64_t                                      nextTaskId,
        const std::vector<int64_t>&                  taskOrder,
        const std::vector<yyjson::value>&            taskInstances)
    {
        yyjson::value index(Das::Utils::MakeYyjsonObject());
        (*index.as_object())[std::string_view("nextTaskId")] = nextTaskId;
        {
            yyjson::value order_arr(Das::Utils::MakeYyjsonArray());
            for (auto id : taskOrder)
            {
                (*order_arr.as_array()).emplace_back(id);
            }
            (*index.as_object())[std::string_view("taskOrder")] =
                std::move(order_arr);
        }
        sm.UpdateSchedulerIndexJson("0", index);

        for (size_t i = 0; i < taskInstances.size() && i < taskOrder.size();
             ++i)
        {
            sm.UpdateTaskInstanceJson("0", taskOrder[i], taskInstances[i]);
        }
    }

    void WriteFactoryPluginManifest(const std::filesystem::path& manifest_path)
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
            yyjson::value factories(Das::Utils::MakeYyjsonArray());
            factories.as_array()->emplace_back(
                FactoryTaskComponentFactoryGuidString);
            (*task_components.as_object())[std::string_view("factories")] =
                std::move(factories);
            (*task_components.as_object())[std::string_view("components")] =
                yyjson::value(Das::Utils::MakeYyjsonObject());
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
                yyjson::value descriptors(Das::Utils::MakeYyjsonArray());
                {
                    yyjson::value desc(Das::Utils::MakeYyjsonObject());
                    (*desc.as_object())[std::string_view("name")] = "key1";
                    (*desc.as_object())[std::string_view("type")] =
                        static_cast<int64_t>(
                            Das::ExportInterface::DAS_TYPE_STRING);
                    (*desc.as_object())[std::string_view("defaultValue")] =
                        "default";
                    descriptors.as_array()->emplace_back(std::move(desc));
                }
                {
                    yyjson::value desc(Das::Utils::MakeYyjsonObject());
                    (*desc.as_object())[std::string_view("name")] =
                        "retryCount";
                    (*desc.as_object())[std::string_view("type")] =
                        static_cast<int64_t>(
                            Das::ExportInterface::DAS_TYPE_INT);
                    (*desc.as_object())[std::string_view("defaultValue")] = 3;
                    descriptors.as_array()->emplace_back(std::move(desc));
                }
                (*task_entry.as_object())[std::string_view("descriptors")] =
                    std::move(descriptors);
            }
            {
                yyjson::value authoring(Das::Utils::MakeYyjsonObject());
                (*authoring.as_object())[std::string_view("factoryGuid")] =
                    FactoryAuthoringFactoryGuidString;
                yyjson::value supported(Das::Utils::MakeYyjsonArray());
                (*supported.as_array()).emplace_back("formSequence");
                (*authoring.as_object())[std::string_view("supportedKinds")] =
                    std::move(supported);
                (*task_entry.as_object())[std::string_view("authoring")] =
                    std::move(authoring);
            }
            yyjson::value tasks_obj(Das::Utils::MakeYyjsonObject());
            (*tasks_obj.as_object())[std::string_view(FactoryTaskGuidString)] =
                std::move(task_entry);
            (*manifest.as_object())[std::string_view("tasks")] =
                std::move(tasks_obj);
        }

        std::ofstream ofs(manifest_path);
        ofs << *Das::Utils::SerializeYyjsonValue(manifest, true);
    }

    // Builds a Phase-84 strict change for repository entries. Repository Apply
    // shares the Task Apply pipeline, so the wire shape is identical; only the
    // surrounding context (entry vs task) differs at the caller.
    yyjson::value MakeEmptyAuthoringRequest()
    {
        return yyjson::value(Das::Utils::MakeYyjsonObject());
    }
} // namespace

class SchedulerAuthoringCacheTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        test_dir_ = UniqueTestDir();
        std::filesystem::create_directories(test_dir_);
        settings_dir_ = test_dir_ / "settings";
        std::filesystem::create_directories(settings_dir_);
        plugin_dir_ = test_dir_ / "plugins";
        std::filesystem::create_directories(plugin_dir_);
        manifest_path_ = plugin_dir_ / "FactoryPlugin.json";
        WriteFactoryPluginManifest(manifest_path_);

        CreateSharedStateSegment();
        DeploySchedulerTestPlugin();

        settings_manager_ =
            std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        settings_manager_->CreateProfile("0");

        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        plugin_manager_ = std::make_unique<
            Das::Core::ForeignInterfaceHost::TestablePluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));
        try
        {
            const auto host_path = IpcTestConfig::GetDasHostPath();
            if (std::filesystem::exists(host_path))
            {
                plugin_manager_->SetHostExePath(host_path);
            }
        }
        catch (const std::exception&)
        {
            // host 未配置时 Initialize 会失败，断言会暴露具体错误。
        }

        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));

        ASSERT_EQ(plugin_manager_->Initialize(1), DAS_S_OK);

        io_thread_ = std::thread([this]() { ipc_sp_->Run(); });
    }

    void CreateSharedStateSegment()
    {
        const uint64_t pid =
#ifdef _WIN32
            static_cast<uint64_t>(::GetCurrentProcessId());
#else
            static_cast<uint64_t>(::getpid());
#endif
        const uint64_t counter = s_shm_counter.fetch_add(1);
        shm_name_ = "DasSchedulerAuthoringCacheTest_" + std::to_string(pid)
                    + "_" + std::to_string(counter);

        boost::interprocess::shared_memory_object::remove(shm_name_.c_str());

        const size_t kShmSize = sizeof(FactoryTaskSharedState) + 4096;
        shm_segment_ =
            std::make_unique<boost::interprocess::managed_shared_memory>(
                boost::interprocess::create_only_t{},
                shm_name_.c_str(),
                kShmSize);

        shared_state_ =
            shm_segment_->construct<FactoryTaskSharedState>("state")();

#ifdef _WIN32
        _putenv_s("DAS_SCHEDULER_TEST_SHM_NAME", shm_name_.c_str());
#else
        setenv("DAS_SCHEDULER_TEST_SHM_NAME", shm_name_.c_str(), 1);
#endif
    }

    void DeploySchedulerTestPlugin()
    {
        namespace fs = std::filesystem;
        const fs::path build_plugin_dir{IpcTestConfig::GetPluginDir()};
#ifndef DAS_SCHEDULER_TEST_PLUGIN_FILENAME
#error "DAS_SCHEDULER_TEST_PLUGIN_FILENAME must be defined via CMake"
#endif
        const fs::path src_dll =
            build_plugin_dir / DAS_SCHEDULER_TEST_PLUGIN_FILENAME;
        ASSERT_TRUE(fs::exists(src_dll))
            << "SchedulerTestPlugin artifact not found at: "
            << src_dll.string();

        const fs::path dst_dll = plugin_dir_ / "FactoryPlugin.dll";
        fs::copy_file(src_dll, dst_dll, fs::copy_options::overwrite_existing);
    }

    void TearDown() override
    {
        if (scheduler_ && scheduler_->Status() == SchedulerState::Running)
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
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }

    /// Persist a single authoring-capable task instance with the given id.
    void SeedAuthoringTask(int64_t task_id)
    {
        yyjson::value task(Das::Utils::MakeYyjsonObject());
        (*task.as_object())[std::string_view("id")] = task_id;
        (*task.as_object())[std::string_view("taskGuid")] =
            FactoryTaskGuidString;
        (*task.as_object())[std::string_view("pluginGuid")] =
            FactoryPluginGuidString;
        (*task.as_object())[std::string_view("properties")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        WriteSchedulerState(*settings_manager_, task_id + 1, {task_id}, {task});
    }

    std::filesystem::path test_dir_;
    std::filesystem::path settings_dir_;
    std::filesystem::path plugin_dir_;
    std::filesystem::path manifest_path_;
    std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
                                                              settings_manager_;
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
    std::unique_ptr<Das::Core::ForeignInterfaceHost::TestablePluginManager>
                                      plugin_manager_;
    std::unique_ptr<SchedulerService> scheduler_;
    FactoryTaskSharedState*           shared_state_ = nullptr;
    std::unique_ptr<boost::interprocess::managed_shared_memory> shm_segment_;
    std::string                                                 shm_name_;
    std::thread                                                 io_thread_;

    static inline std::atomic<uint64_t> s_shm_counter{0};
};

// REQ-06a: a second Get on the same task reuses the cached session — the
// FakeAuthoringSession shared-memory `authoring_session_count` stays at 1.
TEST_F(SchedulerAuthoringCacheTest, SchedulerAuthoringCacheHit)
{
    SeedAuthoringTask(0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request = MakeEmptyAuthoringRequest();

    auto first = scheduler_->GetTaskAuthoringDocument(0, request);
    ASSERT_TRUE(first.as_object().has_value());

    auto second = scheduler_->GetTaskAuthoringDocument(0, request);
    ASSERT_TRUE(second.as_object().has_value());

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->authoring_session_count, 1);
}

// REQ-06b: the session is created lazily on first authoring access — the
// counter is 0 right after Initialize (no eager creation) and becomes 1 after
// the first Get.
TEST_F(SchedulerAuthoringCacheTest, SchedulerAuthoringCacheLazyCreate)
{
    SeedAuthoringTask(0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 0);
    }

    const yyjson::value request = MakeEmptyAuthoringRequest();
    auto result = scheduler_->GetTaskAuthoringDocument(0, request);
    ASSERT_TRUE(result.as_object().has_value());

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->authoring_session_count, 1);
}
