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

#include "TestAdmission.h"

#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasApi.h>
#include <das/DasRef.hpp>
#include <das/DasString.hpp>
#include <das/Plugins/SchedulerTestPlugin/SchedulerTestSharedState.h>
#include <das/Utils/PathText.h>
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
        // 裸计数名在每个测试进程里都从 0 重来：同族不同用例并行时多个
        // 进程会拿同名目录互相 remove_all。掺 PID 保证跨进程唯一，计数
        // 只负责进程内多次调用的区分。
        static std::atomic<int> counter{0};
        const uint64_t          pid =
#ifdef _WIN32
            static_cast<uint64_t>(::GetCurrentProcessId());
#else
            static_cast<uint64_t>(::getpid());
#endif
        auto name = "scheduler_authoring_cache_test_" + std::to_string(pid)
                    + "_" + std::to_string(counter.fetch_add(1));
        auto path =
            std::filesystem::current_path() / Das::Utils::PathFromUtf8(name);
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

    yyjson::value MakeStrictAuthoringChange(
        int64_t          base_revision,
        std::string_view kind = "setValue")
    {
        yyjson::value change(Das::Utils::MakeYyjsonObject());
        (*change.as_object())[std::string_view("baseRevision")] = base_revision;
        yyjson::value element(Das::Utils::MakeYyjsonObject());
        (*element.as_object())[std::string_view("kind")] =
            std::make_pair(std::string_view(kind), yyjson::copy_string);
        yyjson::value payload(Das::Utils::MakeYyjsonObject());
        if (kind == std::string_view("setValue"))
        {
            yyjson::value target(Das::Utils::MakeYyjsonObject());
            (*target.as_object())[std::string_view("kind")] = "document";
            (*payload.as_object())[std::string_view("target")] =
                std::move(target);
            (*payload.as_object())[std::string_view("fieldName")] = "key1";
            (*payload.as_object())[std::string_view("value")] = "edited";
        }
        (*element.as_object())[std::string_view("payload")] =
            std::move(payload);
        auto changes = Das::Utils::MakeYyjsonArray();
        (*changes.as_array()).emplace_back(std::move(element));
        (*change.as_object())[std::string_view("changes")] = std::move(changes);
        return change;
    }

    yyjson::value MakeRepositoryCreateRequest(
        std::string_view display_name,
        int64_t          retry_count = 3)
    {
        yyjson::value request(Das::Utils::MakeYyjsonObject());
        (*request.as_object())[std::string_view("pluginGuid")] =
            std::string(FactoryPluginGuidString);
        (*request.as_object())[std::string_view("taskTypeGuid")] =
            std::string(FactoryTaskGuidString);
        (*request.as_object())[std::string_view("displayName")] =
            std::string(display_name);

        yyjson::value initial(Das::Utils::MakeYyjsonObject());
        (*initial.as_object())[std::string_view("retryCount")] = retry_count;
        (*request.as_object())[std::string_view("initialProperties")] =
            std::move(initial);
        return request;
    }

    int64_t ReadRevision(const yyjson::value& result)
    {
        auto obj = result.as_object();
        if (!obj.has_value() || !obj->contains(std::string_view("revision")))
        {
            return -1;
        }
        return (*obj)[std::string_view("revision")].as_sint().value_or(-1);
    }

    // A Get response carries the revision inside document.revision (only the
    // Apply response exposes a top-level revision). Read it out by path so the
    // strict Get response shape is observed without throwing on the missing
    // top-level key.
    int64_t ReadDocumentRevision(const yyjson::value& result)
    {
        auto obj = result.as_object();
        if (!obj.has_value() || !obj->contains(std::string_view("document")))
        {
            return -1;
        }
        auto doc = (*obj)[std::string_view("document")].as_object();
        if (!doc.has_value() || !doc->contains(std::string_view("revision")))
        {
            return -1;
        }
        return (*doc)[std::string_view("revision")].as_sint().value_or(-1);
    }

    // Reads the persisted authoring revision straight off the settings store
    // (authoring.revision). The SchedulerService GetAuthoringRevision helper is
    // static inside SchedulerService.cpp's anonymous namespace, so the test
    // re-derives the field path here.
    int64_t ReadPersistedTaskRevision(
        Das::Core::SettingsManager::SettingsManager& sm,
        int64_t                                      task_id)
    {
        auto task_json = sm.GetTaskInstanceJson("0", task_id);
        auto obj = task_json.as_object();
        if (!obj.has_value())
        {
            return -1;
        }
        auto authoring = (*obj)[std::string_view("authoring")].as_object();
        if (!authoring.has_value())
        {
            return -1;
        }
        return (*authoring)[std::string_view("revision")].as_sint().value_or(
            -1);
    }

    bool ResultIsOk(const yyjson::value& result)
    {
        auto obj = result.as_object();
        if (!obj.has_value())
        {
            return false;
        }
        return (*obj)[std::string_view("ok")].as_bool().value_or(false);
    }

    bool ResultIsRevisionConflict(const yyjson::value& result)
    {
        auto obj = result.as_object();
        if (!obj.has_value())
        {
            return false;
        }
        return (*obj)[std::string_view("errorKind")].as_string().value_or("")
               == std::string_view("revisionConflict");
    }

    // Builds a minimal authoring-capable task instance with the given id, for
    // tests that seed more than one task (WriteSchedulerState rewrites the
    // index, so multi-task tests must seed all tasks in one call).
    yyjson::value MakeSeededTask(int64_t task_id)
    {
        yyjson::value task(Das::Utils::MakeYyjsonObject());
        (*task.as_object())[std::string_view("id")] = task_id;
        (*task.as_object())[std::string_view("taskGuid")] =
            FactoryTaskGuidString;
        (*task.as_object())[std::string_view("pluginGuid")] =
            FactoryPluginGuidString;
        (*task.as_object())[std::string_view("properties")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        return task;
    }
} // namespace

class SchedulerAuthoringCacheTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        test_dir_ = UniqueTestDir();
        std::filesystem::create_directories(test_dir_);
        settings_dir_ = test_dir_ / DAS_PATH_LITERAL("settings");
        std::filesystem::create_directories(settings_dir_);
        plugin_dir_ = test_dir_ / DAS_PATH_LITERAL("plugins");
        std::filesystem::create_directories(plugin_dir_);
        manifest_path_ = plugin_dir_ / DAS_PATH_LITERAL("FactoryPlugin.json");
        WriteFactoryPluginManifest(manifest_path_);

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
            // host 未配置时 Initialize 会失败，断言会暴露具体错误。
        }

        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_.Get()));

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

        const size_t shm_size = sizeof(FactoryTaskSharedState) + 4096;
        shm_segment_ =
            std::make_unique<boost::interprocess::managed_shared_memory>(
                boost::interprocess::create_only_t{},
                shm_name_.c_str(),
                shm_size);

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
        const fs::path build_plugin_dir = IpcTestConfig::GetPluginDir();
#ifndef DAS_SCHEDULER_TEST_PLUGIN_FILENAME
#error "DAS_SCHEDULER_TEST_PLUGIN_FILENAME must be defined via CMake"
#endif
        const fs::path src_dll =
            build_plugin_dir
            / DAS_PATH_LITERAL(DAS_SCHEDULER_TEST_PLUGIN_FILENAME);
        ASSERT_TRUE(fs::exists(src_dll))
            << "SchedulerTestPlugin artifact not found at: "
            << Das::Utils::PathToUtf8(src_dll);

        const fs::path dst_dll =
            plugin_dir_ / DAS_PATH_LITERAL("FactoryPlugin.dll");
        fs::copy_file(src_dll, dst_dll, fs::copy_options::overwrite_existing);
    }

    void TearDown() override
    {
        if (scheduler_ && scheduler_->Status() == SchedulerState::Running)
        {
            EXPECT_EQ(scheduler_->Disable(), DAS_S_OK);
        }

        // PluginManager 关停会把卸载任务派发到 BusinessThread：io_context
        // 先停会让该任务失去执行载体，非 BT 调用方的泵化等待随之死等
        // （并发下窗口张开即挂起）——Shutdown 必须先于 RequestStop。
        scheduler_.reset();
        if (plugin_manager_)
        {
            plugin_manager_->Shutdown();
        }
        plugin_manager_.reset();
        settings_manager_.reset();

        if (ipc_sp_)
        {
            ipc_sp_->RequestStop();
        }
        if (io_thread_.joinable())
        {
            io_thread_.join();
        }
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

    /// Park the FakeAuthoringSession::ApplyChange on the shared cv. The Apply
    /// then holds the per-session write lock (entry->lock) while waiting, and
    /// releases state_->mutex while parked (cv.wait contract), so a Get on a
    /// different task is not blocked by the fake session's mutex.
    void SetBlockApply(bool value)
    {
        {
            const ipc_scoped_lock lock(shared_state_->mutex);
            shared_state_->block_apply = value;
            shared_state_->apply_entered = false;
            shared_state_->unblock_apply = false;
        }
        shared_state_->cv.notify_all();
    }

    /// Release a parked Apply. Once unblock_apply is set every later Apply in
    /// the same test returns from the wait immediately.
    void UnblockApply()
    {
        {
            const ipc_scoped_lock lock(shared_state_->mutex);
            shared_state_->unblock_apply = true;
        }
        shared_state_->cv.notify_all();
    }

    /// Spin until the parked Apply has entered ApplyChange (apply_entered) or
    /// time out. Guarantees the Apply holds the per-session write lock when it
    /// returns true.
    bool WaitForApplyEntered(
        std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                const ipc_scoped_lock lock(shared_state_->mutex);
                if (shared_state_->apply_entered)
                {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
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
    std::unique_ptr<SchedulerService> scheduler_;
    FactoryTaskSharedState*           shared_state_ = nullptr;
    std::unique_ptr<boost::interprocess::managed_shared_memory> shm_segment_;
    std::string                                                 shm_name_;
    std::thread                                                 io_thread_;

    static inline std::atomic<uint64_t> s_shm_counter{0};
};

// ====== host 申报（准入闸门注册，注册器同时装入 listener） ======
// 申报值=本族用例加载的 loadMode:ipc 插件数（一插件一 host）：夹具只部署
// 探针 FactoryPlugin（1 host）。生产路径 spawn（PluginManager→
// IpcRemotePluginHost）测试侧不可见，故无违约计数（用户裁决：不动生产
// 文件）。插件数变动时同步改申报值。
DAS_TEST_MAX_HOSTS_SUITE(SchedulerAuthoringCacheTest, 1)

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

// Apply path: two consecutive Apply calls on the same task reuse one session
// (authoring_session_count stays 1) while ApplyChange runs twice
// (apply_change_count == 2). Proves the Apply pipeline routes through the cache
// without breaking the strict envelope/revision pre-checks (each Apply still
// advances the revision).
TEST_F(SchedulerAuthoringCacheTest, SchedulerAuthoringApplyCacheHit)
{
    SeedAuthoringTask(0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto first =
        scheduler_->ApplyTaskAuthoringChange(0, MakeStrictAuthoringChange(0));
    ASSERT_TRUE(first.as_object().has_value());
    ASSERT_TRUE(
        (*first.as_object())[std::string_view("ok")].as_bool().value_or(false));
    const int64_t revision_after_first = ReadRevision(first);
    ASSERT_EQ(revision_after_first, 1);

    auto second = scheduler_->ApplyTaskAuthoringChange(
        0,
        MakeStrictAuthoringChange(revision_after_first));
    ASSERT_TRUE(second.as_object().has_value());
    ASSERT_TRUE(
        (*second.as_object())[std::string_view("ok")].as_bool().value_or(
            false));
    ASSERT_EQ(ReadRevision(second), 2);

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->authoring_session_count, 1);
    EXPECT_EQ(shared_state_->apply_change_count, 2);
}

// Compile path: two Compile calls on the same task reuse one session.
TEST_F(SchedulerAuthoringCacheTest, SchedulerAuthoringCompileCacheHit)
{
    SeedAuthoringTask(0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request = MakeEmptyAuthoringRequest();

    auto first = scheduler_->CompileTaskAuthoring(0, request);
    ASSERT_TRUE(first.as_object().has_value());

    auto second = scheduler_->CompileTaskAuthoring(0, request);
    ASSERT_TRUE(second.as_object().has_value());

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->authoring_session_count, 1);
    EXPECT_EQ(shared_state_->compile_count, 2);
}

// Repository Get path: two Get calls on the same entry reuse one session
// (repository_authoring_sessions_ cache).
TEST_F(SchedulerAuthoringCacheTest, SchedulerAuthoringRepositoryEntryCacheHit)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Repository cache hit", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    const yyjson::value request = MakeEmptyAuthoringRequest();

    auto first = scheduler_->GetRepositoryEntryAuthoringDocument(0, request);
    ASSERT_TRUE(first.as_object().has_value());

    auto second = scheduler_->GetRepositoryEntryAuthoringDocument(0, request);
    ASSERT_TRUE(second.as_object().has_value());

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->authoring_session_count, 1);
}

// REQ-07a: two concurrent Applies to the same task are serialized by the
// per-session write lock. Thread A (baseRevision 0) is parked inside
// ApplyChange holding the write lock; thread B (baseRevision 1, building on A's
// expected commit) blocks on the write lock. Once A is released, B re-reads the
// now-bumped revision under the lock, passes the authoritative revision check,
// and commits base+2. Both succeed; the final persisted revision is base+2 (no
// jump, no regression). Without serialization B would read the pre-A revision
// and either conflict or overwrite — never reach base+2 with both ok.
TEST_F(SchedulerAuthoringCacheTest, SchedulerAuthoringConcurrentApplySerialized)
{
    SeedAuthoringTask(0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request = MakeEmptyAuthoringRequest();
    auto prime = scheduler_->GetTaskAuthoringDocument(0, request);
    ASSERT_EQ(ReadDocumentRevision(prime), 0);

    SetBlockApply(true);

    std::atomic<int>     ok_count{0};
    std::atomic<int>     conflict_count{0};
    std::atomic<int64_t> last_revision{-1};
    std::atomic<bool>    a_started{false};

    auto run_apply = [&](int64_t base_revision)
    {
        auto result = scheduler_->ApplyTaskAuthoringChange(
            0,
            MakeStrictAuthoringChange(base_revision));
        if (ResultIsOk(result))
        {
            ++ok_count;
            last_revision = ReadRevision(result);
        }
        else if (ResultIsRevisionConflict(result))
        {
            ++conflict_count;
        }
    };

    std::thread a(
        [&]
        {
            a_started = true;
            run_apply(0);
        });
    ASSERT_TRUE(WaitForApplyEntered());
    // Let A settle into the cv.wait so it holds the write lock.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::thread b([&] { run_apply(1); });
    // B must be blocked behind A's write lock, not complete first.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    UnblockApply();
    a.join();
    b.join();

    EXPECT_EQ(ok_count.load(), 2);
    EXPECT_EQ(conflict_count.load(), 0);
    EXPECT_EQ(last_revision.load(), 2);
    EXPECT_EQ(
        ReadPersistedTaskRevision(*settings_manager_, 0),
        2); // base+2 on disk, no jump/regression

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 2);
}

// REQ-07b: no lost update — both concurrent Applies take effect. With the same
// serialization as REQ-07a (A parked, B queued behind the write lock), the
// provider's ApplyChange runs twice (apply_change_count == 2) and the persisted
// authoring revision advances to base+2. Without serialization B would conflict
// on the stale pre-A revision (apply_change_count == 1, lost update).
TEST_F(
    SchedulerAuthoringCacheTest,
    SchedulerAuthoringConcurrentApplyNoLostUpdate)
{
    SeedAuthoringTask(0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request = MakeEmptyAuthoringRequest();
    auto prime = scheduler_->GetTaskAuthoringDocument(0, request);
    ASSERT_EQ(ReadDocumentRevision(prime), 0);

    SetBlockApply(true);

    std::atomic<int64_t> a_revision{-1};
    std::atomic<int64_t> b_revision{-1};

    std::thread a(
        [&]
        {
            auto r = scheduler_->ApplyTaskAuthoringChange(
                0,
                MakeStrictAuthoringChange(0));
            a_revision = ReadRevision(r);
        });
    ASSERT_TRUE(WaitForApplyEntered());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::thread b(
        [&]
        {
            auto r = scheduler_->ApplyTaskAuthoringChange(
                0,
                MakeStrictAuthoringChange(1));
            b_revision = ReadRevision(r);
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    UnblockApply();
    a.join();
    b.join();

    // A committed base+1, B committed base+2 — both ApplyChange invocations
    // took effect, no lost update.
    EXPECT_EQ(a_revision.load(), 1);
    EXPECT_EQ(b_revision.load(), 2);
    EXPECT_EQ(ReadPersistedTaskRevision(*settings_manager_, 0), 2);

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 2);
    // Apply carries the document in the strict envelope; no GetDocument
    // fallback.
    EXPECT_EQ(shared_state_->get_document_count, 1);
}

// REQ-07c: during a slow Apply (parked holding the per-session write lock), a
// Get on the same task blocks on the write lock and then returns a complete
// snapshot — never a half-applied middle state. The blocked Get reads the old
// committed revision (its settings read predates the persist); a fresh Get
// after the Apply commits reads the new revision via the read-side freshness
// rebuild.
TEST_F(
    SchedulerAuthoringCacheTest,
    SchedulerAuthoringGetReadsConsistentSnapshot)
{
    SeedAuthoringTask(0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request = MakeEmptyAuthoringRequest();
    auto prime = scheduler_->GetTaskAuthoringDocument(0, request);
    ASSERT_EQ(ReadDocumentRevision(prime), 0);

    SetBlockApply(true);

    std::thread apply_thread(
        [&]
        {
            scheduler_->ApplyTaskAuthoringChange(
                0,
                MakeStrictAuthoringChange(0));
        });
    ASSERT_TRUE(WaitForApplyEntered());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Get on the same task while the Apply holds the write lock. It must block
    // (shared_lock waits for the unique_lock) and not observe a partial state.
    std::promise<yyjson::value>       get_promise;
    std::shared_future<yyjson::value> get_future =
        get_promise.get_future().share();
    std::thread get_thread(
        [&]
        {
            get_promise.set_value(
                scheduler_->GetTaskAuthoringDocument(0, request));
        });

    EXPECT_EQ(
        get_future.wait_for(std::chrono::milliseconds(150)),
        std::future_status::timeout)
        << "Get completed during a slow Apply — write lock not held";

    UnblockApply();
    apply_thread.join();

    yyjson::value blocked_get = get_future.get();
    get_thread.join();
    ASSERT_TRUE(blocked_get.as_object().has_value());
    // Complete OLD snapshot: the blocked Get's settings read predates the
    // persist, so the cached session still reports the pre-Apply revision.
    EXPECT_EQ(ReadDocumentRevision(blocked_get), 0);

    // After the Apply committed, a fresh Get rebuilds (read-side freshness) and
    // reads the complete NEW snapshot.
    auto after_get = scheduler_->GetTaskAuthoringDocument(0, request);
    EXPECT_EQ(ReadDocumentRevision(after_get), 1);
}

// REQ-07d: per-session lock granularity — authoring sessions are cached
// per-task, so a Get on one task reads its own snapshot through its own session
// and never cross-contaminates with another task's revision. Two tasks seeded
// with distinct revisions must each observe their own revision, and the plugin
// must report two separate sessions (one per task).
//
// The SchedulerTestPlugin runs in a single DasHost subprocess whose one
// business thread services one IPC call at a time, so end-to-end Get/Apply
// parallelism cannot be timed through IPC. The per-session shared_lock
// granularity itself is exercised under load by REQ-07a/b/c; this test pins
// the per-task isolation guarantee those tests rely on.
TEST_F(SchedulerAuthoringCacheTest, SchedulerAuthoringGetParallelDifferentTasks)
{
    // Seed both tasks in one shot: WriteSchedulerState rewrites the index
    // (taskOrder), so two separate SeedAuthoringTask calls would clobber the
    // first task out of the order. Task 0 has no authoring (revision 0); task 1
    // carries revision 5 so per-task isolation is observable.
    auto make_task = [](int64_t task_id, std::optional<int64_t> revision)
    {
        yyjson::value task(Das::Utils::MakeYyjsonObject());
        (*task.as_object())[std::string_view("id")] = task_id;
        (*task.as_object())[std::string_view("taskGuid")] =
            FactoryTaskGuidString;
        (*task.as_object())[std::string_view("pluginGuid")] =
            FactoryPluginGuidString;
        (*task.as_object())[std::string_view("properties")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        if (revision.has_value())
        {
            yyjson::value authoring(Das::Utils::MakeYyjsonObject());
            (*authoring.as_object())[std::string_view("revision")] = *revision;
            (*task.as_object())[std::string_view("authoring")] =
                std::move(authoring);
        }
        return task;
    };
    WriteSchedulerState(
        *settings_manager_,
        2,
        {0, 1},
        {make_task(0, std::nullopt), make_task(1, int64_t{5})});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request = MakeEmptyAuthoringRequest();

    auto get0 = scheduler_->GetTaskAuthoringDocument(0, request);
    auto get1 = scheduler_->GetTaskAuthoringDocument(1, request);

    ASSERT_TRUE(get0.as_object().has_value());
    ASSERT_TRUE(get1.as_object().has_value());
    EXPECT_EQ(ReadDocumentRevision(get0), 0);
    EXPECT_EQ(ReadDocumentRevision(get1), 5);

    // Per-task sessions: one cached entry per task id, no shared session.
    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->authoring_session_count, 2);
}

// REQ-06c: DeleteTask evicts the cached authoring session for that task only
// (scoped evict, not a global clear). Seed two tasks, populate both sessions,
// then delete task 0 and Get task 1 — task 1's cached session must still be a
// hit (count stays 2). A buggy global clear would force task 1 to rebuild
// (count 3). (Re-creating task 0 to observe its own rebuild end-to-end requires
// re-Initialize, which hits a separate pre-existing PluginManager UAF outside
// this plan's scope — see deferred-items; the scoped guarantee plus the
// EvictAuthoringSession `cache.erase(id)` impl document the per-id evict.)
TEST_F(SchedulerAuthoringCacheTest, SchedulerAuthoringCacheEvictOnDelete)
{
    WriteSchedulerState(
        *settings_manager_,
        2,
        {0, 1},
        {MakeSeededTask(0), MakeSeededTask(1)});
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request = MakeEmptyAuthoringRequest();
    scheduler_->GetTaskAuthoringDocument(0, request);
    scheduler_->GetTaskAuthoringDocument(1, request);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 2);
    }

    ASSERT_EQ(scheduler_->DeleteTask(0), DAS_S_OK);

    // Task 1's session is untouched by task 0's eviction — still a cache hit.
    scheduler_->GetTaskAuthoringDocument(1, request);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 2);
    }
}

// REQ-06c (repository side): DeleteRepositoryEntry evicts the cached repository
// authoring session scoped to that entry. Two entries, both Get-populated,
// delete entry 0, then Get entry 1 stays a cache hit (count stays 2).
TEST_F(
    SchedulerAuthoringCacheTest,
    SchedulerAuthoringRepositoryCacheEvictOnDelete)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto created0 = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Repo entry 0", 3));
    ASSERT_EQ(
        (*created0.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);
    auto created1 = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Repo entry 1", 3));
    ASSERT_EQ(
        (*created1.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        1);

    const yyjson::value request = MakeEmptyAuthoringRequest();
    scheduler_->GetRepositoryEntryAuthoringDocument(0, request);
    scheduler_->GetRepositoryEntryAuthoringDocument(1, request);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 2);
    }

    ASSERT_EQ(scheduler_->DeleteRepositoryEntry(0), DAS_S_OK);

    // Entry 1's session is untouched by entry 0's eviction — still a cache hit.
    scheduler_->GetRepositoryEntryAuthoringDocument(1, request);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 2);
    }
}

// REQ-06d / REQ-08: the public invalidation entry drops the cached session so
// the next Get rebuilds (count 1 -> 2).
TEST_F(
    SchedulerAuthoringCacheTest,
    SchedulerAuthoringInvalidateAuthoringSessions)
{
    SeedAuthoringTask(0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request = MakeEmptyAuthoringRequest();
    scheduler_->GetTaskAuthoringDocument(0, request);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 1);
    }

    scheduler_->InvalidateAuthoringSessions();

    scheduler_->GetTaskAuthoringDocument(0, request);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 2);
    }
}

// REQ-06d: Initialize reentry clears the authoring cache while holding the
// SchedulerService mutex. PluginManager's synchronous unload notification must
// not try to acquire that mutex again.
TEST_F(SchedulerAuthoringCacheTest, SchedulerAuthoringInvalidateOnReinitialize)
{
    SeedAuthoringTask(0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request = MakeEmptyAuthoringRequest();
    scheduler_->GetTaskAuthoringDocument(0, request);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 1);
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    scheduler_->GetTaskAuthoringDocument(0, request);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 2);
    }
}

// REQ-08 (mechanism, the real deliverable): the PluginManager ->
// SchedulerService listener chain clears the authoring cache. Calls the
// listener entry point (OnPluginLifecycleEvent) directly — bypassing the
// unload path's pre-existing GetFeaturesByType UAF (see deferred-items.md) —
// and observes authoring_session_count 1 -> 2 on the next Get, proving
// OnPluginLifecycleEvent -> InvalidateAuthoringSessions fires. Both phases
// (Loaded / Unloading) and any guid trigger the coarse full-clear (D-87-08).
TEST_F(
    SchedulerAuthoringCacheTest,
    SchedulerAuthoringInvalidateOnPluginLifecycleEvent)
{
    SeedAuthoringTask(0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request = MakeEmptyAuthoringRequest();
    scheduler_->GetTaskAuthoringDocument(0, request);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 1);
    }

    // Unloading phase on an arbitrary guid -> coarse clear.
    DasGuid arbitrary_guid{};
    scheduler_->OnPluginLifecycleEvent(
        Das::Core::ForeignInterfaceHost::PluginLifecyclePhase::Unloading,
        arbitrary_guid);

    scheduler_->GetTaskAuthoringDocument(0, request);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 2);
    }

    // Loaded phase also triggers the coarse clear (D-87-08 does not
    // distinguish). Count should bump again on the next Get.
    scheduler_->OnPluginLifecycleEvent(
        Das::Core::ForeignInterfaceHost::PluginLifecyclePhase::Loaded,
        arbitrary_guid);

    scheduler_->GetTaskAuthoringDocument(0, request);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 3);
    }
}

// REQ-08 (full E2E): IPC plugin unload fires the Unloading lifecycle event
// (post mutex_ release) -> SchedulerService::OnPluginLifecycleEvent ->
// InvalidateAuthoringSessions clears the cache. The previously-blocking
// pre-existing GetFeaturesByType UAF (feature_type_index_ left dangling after
// UnloadPlugin) is now fixed — CleanupPluginIndexes cleans the index in all
// three unload paths before features are freed. Completing without an ASAN
// abort is the E2E win; the rebuild count is not asserted because the Host
// process is gone post-unload (REQ-08 rebuild verification lives in the
// mechanism test SchedulerAuthoringInvalidateOnPluginLifecycleEvent above).
TEST_F(SchedulerAuthoringCacheTest, SchedulerAuthoringInvalidateOnPluginUnload)
{
    SeedAuthoringTask(0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request = MakeEmptyAuthoringRequest();
    scheduler_->GetTaskAuthoringDocument(0, request);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 1);
    }

    // IPC plugin (manifest loadMode: ipc) -> UnloadPlugin routes to
    // UnloadPluginIpc, which fires the Unloading event after releasing mutex_.
    // The listener (SchedulerService::OnPluginLifecycleEvent) then calls
    // InvalidateAuthoringSessions, clearing the cache. Completing without an
    // ASAN UAF abort is the E2E win here.
    ASSERT_EQ(plugin_manager_->UnloadPlugin(manifest_path_), DAS_S_OK);

    // The cache was cleared by the listener. The post-unload Get cannot
    // rebuild (the Host process is gone — no plugin to re-probe), so we do not
    // assert a count bump — reaching here without an ASAN abort is the signal
    // that the unload path (CleanupPluginIndexes + the fire point in
    // UnloadPluginIpc) is UAF-free for this path. (REQ-08 rebuild verification
    // lives in the mechanism test above.)
    scheduler_->GetTaskAuthoringDocument(0, request);
    SUCCEED() << "unload -> listener fire -> cache clear path executed";
}
