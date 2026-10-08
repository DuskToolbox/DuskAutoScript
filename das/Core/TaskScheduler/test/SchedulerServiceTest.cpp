#include "controller/DasProfileController.hpp"
#include "controller/DasSchedulerController.hpp"
#include <IpcTestConfig.h>
#include <Windows.h>

#include "TestAdmission.h"

#include <das/Core/ForeignInterfaceHost/ErrorLensManager.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/SettingsManager/SettingsServiceImpl.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasApi.h>
#include <das/DasRef.hpp>
#include <das/DasString.hpp>
#include <das/IDasSettingsService.h>
#include <das/Plugins/SchedulerTestPlugin/SchedulerTestSharedState.h>
#include <das/Utils/Expected.h>
#include <das/Utils/PathText.h>
#include <das/_autogen/idl/abi/IDasGuidVector.h>
#include <das/_autogen/idl/abi/IDasLogRecorder.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProcessLogRecorder.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasErrorLens.Implements.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <das/Utils/DasJsonCore.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
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
        auto name = "scheduler_test_" + std::to_string(pid) + "_"
                    + std::to_string(counter.fetch_add(1));
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

    ::testing::AssertionResult WaitForSchedulerStopped(
        SchedulerService&         scheduler,
        std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (scheduler.Status() == SchedulerState::Stopped)
            {
                return ::testing::AssertionSuccess();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return ::testing::AssertionFailure()
               << "scheduler did not reach Stopped within " << timeout.count()
               << "ms; last status=" << static_cast<int>(scheduler.Status());
    }

    // Phase 84 strict Authoring wire test helpers. Defined early so that both
    // the pre-existing authoring tests (updated for strict wire) and the new
    // SchedulerAuthoring*/SchedulerRepositoryAuthoring* cases can call them.
    void EnableStrictAuthoringWire(FactoryTaskSharedState& state)
    {
        const ipc_scoped_lock lock(state.mutex);
        state.emit_strict_wire = true;
    }

    void SeedEmptyAuthoringTask(
        Das::Core::SettingsManager::SettingsManager& sm,
        int64_t                                      task_id = 0,
        int64_t                                      revision = 0,
        std::string_view                             key1_value = {})
    {
        yyjson::value task(Das::Utils::MakeYyjsonObject());
        (*task.as_object())[std::string_view("id")] = task_id;
        (*task.as_object())[std::string_view("taskGuid")] =
            FactoryTaskGuidString;
        (*task.as_object())[std::string_view("pluginGuid")] =
            FactoryPluginGuidString;
        {
            yyjson::value props(Das::Utils::MakeYyjsonObject());
            if (!key1_value.empty())
            {
                (*props.as_object())[std::string_view("key1")] =
                    std::string(key1_value);
            }
            (*task.as_object())[std::string_view("properties")] =
                std::move(props);
        }
        if (revision > 0)
        {
            yyjson::value authoring(Das::Utils::MakeYyjsonObject());
            (*authoring.as_object())[std::string_view("revision")] = revision;
            (*task.as_object())[std::string_view("authoring")] =
                std::move(authoring);
        }
        WriteSchedulerState(sm, task_id + 1, {task_id}, {task});
    }

    // Builds a Phase 84 strict change. Default kind is setValue because that is
    // the most exercise-heavy payload (target/fieldName/value). invokeCommand
    // callers pass the kind explicitly. 批量 apply wire：单 change 经
    // changes[] 包装提交。
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
        else if (kind == std::string_view("invokeCommand"))
        {
            (*payload.as_object())[std::string_view("commandId")] = "test";
        }
        (*element.as_object())[std::string_view("payload")] =
            std::move(payload);
        auto changes = Das::Utils::MakeYyjsonArray();
        (*changes.as_array()).emplace_back(std::move(element));
        (*change.as_object())[std::string_view("changes")] = std::move(changes);
        return change;
    }

    // 批量 apply 请求构造：kinds 逐元素生成与
    // MakeStrictAuthoringChange 同形的 change（setValue/invokeCommand 载荷）。
    yyjson::value MakeStrictAuthoringBatchRequest(
        int64_t                                 base_revision,
        std::initializer_list<std::string_view> kinds)
    {
        yyjson::value change(Das::Utils::MakeYyjsonObject());
        (*change.as_object())[std::string_view("baseRevision")] = base_revision;
        auto changes = Das::Utils::MakeYyjsonArray();
        for (auto kind : kinds)
        {
            yyjson::value element(Das::Utils::MakeYyjsonObject());
            (*element.as_object())[std::string_view("kind")] =
                std::make_pair(kind, yyjson::copy_string);
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
            else if (kind == std::string_view("invokeCommand"))
            {
                (*payload.as_object())[std::string_view("commandId")] = "test";
            }
            (*element.as_object())[std::string_view("payload")] =
                std::move(payload);
            (*changes.as_array()).emplace_back(std::move(element));
        }
        (*change.as_object())[std::string_view("changes")] = std::move(changes);
        return change;
    }
} // namespace

class SchedulerServiceTest : public ::testing::Test
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

        settings_manager_ =
            std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        auto ipc_sp = DAS::Core::IPC::MainProcess::CreateIpcContext(false);
        plugin_manager_ = std::make_unique<
            Das::Core::ForeignInterfaceHost::TestablePluginManager>(
            *settings_manager_,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp.Get()));
        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp.Get()));
    }

    void TearDown() override
    {
        scheduler_.reset();
        plugin_manager_.reset();
        settings_manager_.reset();
        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path test_dir_;
    std::filesystem::path settings_dir_;
    std::filesystem::path plugin_dir_;
    std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
        settings_manager_;
    std::unique_ptr<Das::Core::ForeignInterfaceHost::TestablePluginManager>
                                      plugin_manager_;
    std::unique_ptr<SchedulerService> scheduler_;
};

// ============================================================
// State machine
// ============================================================

TEST_F(SchedulerServiceTest, Status_InitiallyStopped)
{
    EXPECT_EQ(scheduler_->Status(), SchedulerState::Stopped);
}

TEST_F(SchedulerServiceTest, Initialize_SetsUpSuccessfully)
{
    auto result = scheduler_->Initialize(plugin_dir_, {});
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(scheduler_->Status(), SchedulerState::Stopped);
    EXPECT_TRUE(scheduler_->IsInitialized());
}

TEST_F(SchedulerServiceTest, Disable_WhenStopped_ReturnsError)
{
    auto result = scheduler_->Disable();
    EXPECT_NE(result, DAS_S_OK);
    EXPECT_EQ(scheduler_->Status(), SchedulerState::Stopped);
}

TEST_F(SchedulerServiceTest, Initialize_NonExistentDir_ReturnsError)
{
    auto bad_dir = test_dir_ / DAS_PATH_LITERAL("nonexistent_plugins");
    auto result = scheduler_->Initialize(bad_dir, {});
    EXPECT_EQ(result, DAS_E_FAIL);
}

TEST_F(SchedulerServiceTest, Initialize_WithDisabledGuids_Succeeds)
{
    std::vector<DasGuid> disabled_guids;
    const DasGuid        empty_guid{};
    disabled_guids.push_back(empty_guid);

    auto result = scheduler_->Initialize(plugin_dir_, disabled_guids);
    EXPECT_EQ(result, DAS_S_OK);
}

// ============================================================
// No-load-before-initialize
// ============================================================

TEST_F(SchedulerServiceTest, NoPluginLoadBeforeInitialize)
{
    auto features = plugin_manager_->GetFeaturesByType(
        Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK);
    EXPECT_EQ(features.size(), 0u);

    auto state = scheduler_->Get();
    EXPECT_TRUE((*state.as_object()).contains(std::string_view("state")));
    EXPECT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        0u);
    EXPECT_EQ(
        (*state.as_object())[std::string_view("availableTaskTypes")]
            .as_array()
            ->size(),
        0u);
}

TEST_F(SchedulerServiceTest, SchedulerRepositoryGetBeforeInitializeRejected)
{
    auto repository = scheduler_->GetTaskRepository();
    auto obj = repository.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("notInitialized"));
}

TEST_F(SchedulerServiceTest, SchedulerRepositoryGetAfterInitializeEmpty)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto repository = scheduler_->GetTaskRepository();
    auto obj = repository.as_object();
    ASSERT_TRUE(obj.has_value());
    auto entries = (*obj)[std::string_view("entries")].as_array();
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0u);
}

TEST_F(SchedulerServiceTest, Enable_WithoutInitialize_ReturnsError)
{
    auto result = scheduler_->Enable();
    EXPECT_NE(result, DAS_S_OK);
    EXPECT_EQ(scheduler_->Status(), SchedulerState::Stopped);
}

TEST_F(SchedulerServiceTest, Enable_EmptyTasks_ReturnsError)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto result = scheduler_->Enable();
    EXPECT_EQ(result, DAS_E_OBJECT_NOT_INIT);
    EXPECT_EQ(scheduler_->Status(), SchedulerState::Stopped);
}

// ============================================================
// Materialize persisted instances
// ============================================================

TEST_F(SchedulerServiceTest, Initialize_MaterializesPersistedInstances)
{
    settings_manager_->CreateProfile("0");

    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "00000000-0000-0000-0000-000000000001";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task0.as_object())[std::string_view("nextExecutionTime")] =
        yyjson::value{};
    {
        yyjson::value p(Das::Utils::MakeYyjsonObject());
        (*p.as_object())[std::string_view("key1")] = "value1";
        (*task0.as_object())[std::string_view("properties")] = std::move(p);
    }
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    auto result = scheduler_->Initialize(plugin_dir_, {});
    ASSERT_EQ(result, DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        0);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "unavailable");
    EXPECT_EQ(
        (*(*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
                .as_object())[std::string_view("properties")]
              .as_object())[std::string_view("key1")]
            .as_string()
            .value(),
        "value1");
}

TEST_F(SchedulerServiceTest, Initialize_CorruptTaskFile_VisibleAsInvalid)
{
    settings_manager_->CreateProfile("0");

    yyjson::value scheduler_index(Das::Utils::MakeYyjsonObject());
    (*scheduler_index.as_object())[std::string_view("nextTaskId")] = 1;
    {
        yyjson::value arr(Das::Utils::MakeYyjsonArray());
        (*arr.as_array()).emplace_back(0);
        (*scheduler_index.as_object())[std::string_view("taskOrder")] =
            std::move(arr);
    }
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    auto task_file = settings_dir_ / DAS_PATH_LITERAL("0")
                     / DAS_PATH_LITERAL("taskId0.json");
    {
        std::ofstream ofs{task_file};
        ofs << "CORRUPT DATA {{{";
    }

    auto result = scheduler_->Initialize(plugin_dir_, {});
    ASSERT_EQ(result, DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        0);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "invalid");
    auto reason =
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("unavailabilityReason")]
            .as_string();
    ASSERT_TRUE(reason.has_value());
    EXPECT_FALSE(reason->empty());
}

TEST_F(SchedulerServiceTest, Initialize_MissingTaskType_Unavailable)
{
    settings_manager_->CreateProfile("0");

    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-1111-1111-1111-111111111111";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());

    yyjson::value task1(Das::Utils::MakeYyjsonObject());
    (*task1.as_object())[std::string_view("id")] = 1;
    (*task1.as_object())[std::string_view("taskGuid")] =
        "22222222-2222-2222-2222-222222222222";
    (*task1.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task1.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());

    WriteSchedulerState(*settings_manager_, 2, {0, 1}, {task0, task1});

    auto result = scheduler_->Initialize(plugin_dir_, {});
    ASSERT_EQ(result, DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        2u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "unavailable");
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[1]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "unavailable");
}

// ============================================================
// Initialize while Running
// ============================================================

TEST_F(SchedulerServiceTest, Initialize_WhileRunning_Rejected)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    // Cannot transition to Running without real tasks, so the state
    // remains Stopped. The Running rejection path is exercised in
    // production when Initialize is called during an active scheduler.
}

// ============================================================
// Get: merged frontend view
// ============================================================

TEST_F(SchedulerServiceTest, Get_ReturnsMergedView)
{
    settings_manager_->CreateProfile("0");

    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "FFFFFFFF-0000-0000-0000-000000000000";
    // 2026-04-23T12:30:00+08:00 = 2026-04-23T04:30:00Z = 1776918600
    (*task0.as_object())[std::string_view("nextExecutionTime")] = 1776918600;
    {
        yyjson::value p(Das::Utils::MakeYyjsonObject());
        (*p.as_object())[std::string_view("setting1")] = "value1";
        (*p.as_object())[std::string_view("setting2")] = 42;
        (*task0.as_object())[std::string_view("properties")] = std::move(p);
    }
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    EXPECT_TRUE((*state.as_object()).contains(std::string_view("state")));
    EXPECT_TRUE((*state.as_object()).contains(std::string_view("tasks")));
    EXPECT_TRUE(
        (*state.as_object()).contains(std::string_view("availableTaskTypes")));

    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        0);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        1776918600);
    EXPECT_EQ(
        (*(*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
                .as_object())[std::string_view("properties")]
              .as_object())[std::string_view("setting1")]
            .as_string()
            .value(),
        "value1");
    EXPECT_EQ(
        (*(*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
                .as_object())[std::string_view("properties")]
              .as_object())[std::string_view("setting2")]
            .as_sint()
            .value(),
        42);
}

TEST_F(SchedulerServiceTest, Get_CorruptTaskFile_VisibleWithInvalidMarker)
{
    settings_manager_->CreateProfile("0");

    yyjson::value scheduler_index(Das::Utils::MakeYyjsonObject());
    (*scheduler_index.as_object())[std::string_view("nextTaskId")] = 2;
    {
        yyjson::value arr(Das::Utils::MakeYyjsonArray());
        (*arr.as_array()).emplace_back(0);
        (*arr.as_array()).emplace_back(1);
        (*scheduler_index.as_object())[std::string_view("taskOrder")] =
            std::move(arr);
    }
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    // Task 0: corrupt file
    auto task_file = settings_dir_ / DAS_PATH_LITERAL("0")
                     / DAS_PATH_LITERAL("taskId0.json");
    {
        std::ofstream ofs{task_file};
        ofs << "{broken json";
    }

    // Task 1: valid file
    yyjson::value task1(Das::Utils::MakeYyjsonObject());
    (*task1.as_object())[std::string_view("id")] = 1;
    (*task1.as_object())[std::string_view("taskGuid")] =
        "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    (*task1.as_object())[std::string_view("pluginGuid")] =
        "FFFFFFFF-0000-0000-0000-000000000000";
    {
        yyjson::value p(Das::Utils::MakeYyjsonObject());
        (*p.as_object())[std::string_view("key")] = "value";
        (*task1.as_object())[std::string_view("properties")] = std::move(p);
    }
    settings_manager_->UpdateTaskInstanceJson("0", 1, task1);

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        2u);

    // Corrupt task should be visible as invalid
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        0);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "invalid");

    // Valid task should be visible as unavailable (no plugin loaded)
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[1]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        1);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[1]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "unavailable");
    EXPECT_EQ(
        (*(*(*(*state.as_object())[std::string_view("tasks")].as_array())[1]
                .as_object())[std::string_view("properties")]
              .as_object())[std::string_view("key")]
            .as_string()
            .value(),
        "value");
}

// ============================================================
// Status read
// ============================================================

TEST_F(SchedulerServiceTest, Status_ReturnsCorrectState)
{
    EXPECT_EQ(scheduler_->Status(), SchedulerState::Stopped);

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EXPECT_EQ(scheduler_->Status(), SchedulerState::Stopped);
}

// ============================================================
// Delete task
// ============================================================

TEST_F(SchedulerServiceTest, DeleteTask_PersistsRemoval)
{
    settings_manager_->CreateProfile("0");

    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "FFFFFFFF-0000-0000-0000-000000000000";
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());

    yyjson::value task1(Das::Utils::MakeYyjsonObject());
    (*task1.as_object())[std::string_view("id")] = 1;
    (*task1.as_object())[std::string_view("taskGuid")] =
        "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    (*task1.as_object())[std::string_view("pluginGuid")] =
        "FFFFFFFF-0000-0000-0000-000000000000";
    (*task1.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());

    WriteSchedulerState(*settings_manager_, 2, {0, 1}, {task0, task1});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Delete task 0
    auto result = scheduler_->DeleteTask(0);
    EXPECT_EQ(result, DAS_S_OK);

    // Verify in-memory state
    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        1);

    // Verify persisted state: taskId0.json should be deleted
    EXPECT_FALSE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskId0.json")));
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskId1.json")));

    auto index = settings_manager_->GetSchedulerIndexJson("0");
    ASSERT_EQ(
        (*index.as_object())[std::string_view("taskOrder")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*index.as_object())[std::string_view("taskOrder")].as_array())[0]
            .as_sint()
            .value(),
        1);
}

TEST_F(SchedulerServiceTest, DeleteTask_NotFound)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto result = scheduler_->DeleteTask(99);
    EXPECT_EQ(result, DAS_E_NOT_FOUND);
}

TEST_F(SchedulerServiceTest, DeleteTask_InvalidInstance_Succeeds)
{
    settings_manager_->CreateProfile("0");

    yyjson::value scheduler_index(Das::Utils::MakeYyjsonObject());
    (*scheduler_index.as_object())[std::string_view("nextTaskId")] = 1;
    {
        yyjson::value arr(Das::Utils::MakeYyjsonArray());
        (*arr.as_array()).emplace_back(0);
        (*scheduler_index.as_object())[std::string_view("taskOrder")] =
            std::move(arr);
    }
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    // Corrupt task file
    auto task_file = settings_dir_ / DAS_PATH_LITERAL("0")
                     / DAS_PATH_LITERAL("taskId0.json");
    {
        std::ofstream ofs{task_file};
        ofs << "corrupt";
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "invalid");

    // Deleting an invalid instance should succeed
    auto result = scheduler_->DeleteTask(0);
    EXPECT_EQ(result, DAS_S_OK);

    state = scheduler_->Get();
    EXPECT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        0u);
}

TEST_F(SchedulerServiceTest, DeleteTask_UnavailableInstance_Succeeds)
{
    settings_manager_->CreateProfile("0");

    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-1111-1111-1111-111111111111";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "unavailable");

    auto result = scheduler_->DeleteTask(0);
    EXPECT_EQ(result, DAS_S_OK);

    state = scheduler_->Get();
    EXPECT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        0u);
}

TEST_F(SchedulerServiceTest, DeleteTask_PersistenceFailure_RollsBack)
{
    settings_manager_->CreateProfile("0");

    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-1111-1111-1111-111111111111";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Verify the instance exists
    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);

    // Replace the profile directory with a regular file so that
    // create_directories fails and WriteJsonFile returns an error.
    auto profile_dir = settings_dir_ / DAS_PATH_LITERAL("0");
    std::filesystem::remove_all(profile_dir);
    {
        std::ofstream ofs{profile_dir};
        ofs << "blocker";
    }

    auto result = scheduler_->DeleteTask(0);
    // Should fail because persistence failed
    EXPECT_TRUE(DAS::IsFailed(result));

    // The instance should be restored in memory
    state = scheduler_->Get();
    EXPECT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        0);
}

// ============================================================
// UpdateTaskInternalProperties
// ============================================================

TEST_F(SchedulerServiceTest, UpdateInternalProperties_NextExecutionTime)
{
    settings_manager_->CreateProfile("0");

    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-1111-1111-1111-111111111111";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task0.as_object())[std::string_view("nextExecutionTime")] =
        yyjson::value{};
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Update nextExecutionTime
    // 2026-05-01T08:00:00+08:00 = 2026-05-01T00:00:00Z = 1777593600
    yyjson::value internal_props(Das::Utils::MakeYyjsonObject());
    (*internal_props.as_object())[std::string_view("nextExecutionTime")] =
        1777593600;
    auto result = scheduler_->UpdateTaskInternalProperties(0, internal_props);
    EXPECT_EQ(result, DAS_S_OK);

    // Verify in-memory
    auto state = scheduler_->Get();
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        1777593600);

    // Verify persisted
    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    EXPECT_EQ(
        (*persisted.as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        1777593600);
}

TEST_F(SchedulerServiceTest, UpdateInternalProperties_ClearNextExecutionTime)
{
    settings_manager_->CreateProfile("0");

    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-1111-1111-1111-111111111111";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    // 2026-05-01T08:00:00+08:00 = 2026-05-01T00:00:00Z = 1777593600
    (*task0.as_object())[std::string_view("nextExecutionTime")] = 1777593600;
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    yyjson::value internal_props(Das::Utils::MakeYyjsonObject());
    (*internal_props.as_object())[std::string_view("nextExecutionTime")] =
        nullptr;
    auto result = scheduler_->UpdateTaskInternalProperties(0, internal_props);
    EXPECT_EQ(result, DAS_S_OK);

    auto state = scheduler_->Get();
    EXPECT_TRUE(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("nextExecutionTime")]
            .is_null());
}

TEST_F(SchedulerServiceTest, UnparseableNextExecutionTime_PreservedInGet)
{
    settings_manager_->CreateProfile("0");

    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-2222-3333-4444-555555555555";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000000";
    (*task0.as_object())[std::string_view("nextExecutionTime")] =
        -1; // Invalid timestamp, preserved as-is
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());

    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Negative timestamps are preserved as-is in Get() output.
    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "unavailable");
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        -1);
}

TEST_F(SchedulerServiceTest, UpdateInternalProperties_NotFound)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    yyjson::value internal_props(Das::Utils::MakeYyjsonObject());
    (*internal_props.as_object())[std::string_view("nextExecutionTime")] =
        1777593600;
    auto result = scheduler_->UpdateTaskInternalProperties(99, internal_props);
    EXPECT_EQ(result, DAS_E_NOT_FOUND);
}

// ============================================================
// UpdateTaskProperties on unavailable/invalid instances
// ============================================================

TEST_F(SchedulerServiceTest, UpdateProperties_UnavailableInstance_ReturnsError)
{
    settings_manager_->CreateProfile("0");

    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-1111-1111-1111-111111111111";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    yyjson::value props(Das::Utils::MakeYyjsonObject());
    (*props.as_object())[std::string_view("someKey")] = "someValue";
    auto result = scheduler_->UpdateTaskProperties(0, props);
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(SchedulerServiceTest, UpdateProperties_InvalidInstance_ReturnsError)
{
    settings_manager_->CreateProfile("0");

    yyjson::value scheduler_index(Das::Utils::MakeYyjsonObject());
    (*scheduler_index.as_object())[std::string_view("nextTaskId")] = 1;
    {
        yyjson::value arr(Das::Utils::MakeYyjsonArray());
        (*arr.as_array()).emplace_back(0);
        (*scheduler_index.as_object())[std::string_view("taskOrder")] =
            std::move(arr);
    }
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    auto task_file = settings_dir_ / DAS_PATH_LITERAL("0")
                     / DAS_PATH_LITERAL("taskId0.json");
    {
        std::ofstream ofs{task_file};
        ofs << "corrupt";
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    yyjson::value props(Das::Utils::MakeYyjsonObject());
    (*props.as_object())[std::string_view("someKey")] = "someValue";
    auto result = scheduler_->UpdateTaskProperties(0, props);
    EXPECT_NE(result, DAS_S_OK);
}

// ============================================================
// Running mutation guards
// ============================================================

TEST_F(SchedulerServiceTest, AddTask_NotInitialized_ReturnsError)
{
    const DasGuid task_guid{};
    int64_t       out_id = -1;
    auto          result = scheduler_->AddTask(task_guid, &out_id);
    EXPECT_EQ(result, DAS_E_OBJECT_NOT_INIT);
}

TEST_F(SchedulerServiceTest, AddTask_NotFound)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    DasGuid task_guid{};
    task_guid.data1 = 0xDEADBEEF;
    int64_t out_id = -1;
    auto    result = scheduler_->AddTask(task_guid, &out_id);
    EXPECT_EQ(result, DAS_E_NOT_FOUND);
}

TEST_F(SchedulerServiceTest, DeleteTask_NotInitialized)
{
    auto result = scheduler_->DeleteTask(0);
    // Should not crash. Behavior may be DAS_E_NOT_FOUND since
    // there are no instances.
}

TEST_F(SchedulerServiceTest, UpdateProperties_NotFound)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    yyjson::value props(Das::Utils::MakeYyjsonObject());
    (*props.as_object())[std::string_view("key")] = "value";
    auto result = scheduler_->UpdateTaskProperties(99, props);
    EXPECT_EQ(result, DAS_E_NOT_FOUND);
}

TEST_F(SchedulerServiceTest, AddTask_NullPointer)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const DasGuid task_guid{};
    auto          result = scheduler_->AddTask(task_guid, nullptr);
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);
}

// ============================================================
// Persistence verification
// ============================================================

TEST_F(SchedulerServiceTest, UpdateInternalProperties_PersistsToFile)
{
    settings_manager_->CreateProfile("0");

    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-1111-1111-1111-111111111111";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task0.as_object())[std::string_view("nextExecutionTime")] =
        yyjson::value{};
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    yyjson::value internal_props(Das::Utils::MakeYyjsonObject());
    // 2026-06-01T10:00:00+08:00 = 2026-06-01T02:00:00Z = 1780279200
    (*internal_props.as_object())[std::string_view("nextExecutionTime")] =
        1780279200;
    ASSERT_EQ(
        scheduler_->UpdateTaskInternalProperties(0, internal_props),
        DAS_S_OK);

    // Re-initialize a new scheduler to verify persistence
    auto ipc_sp = DAS::Core::IPC::MainProcess::CreateIpcContext(false);
    auto scheduler2 = std::make_unique<SchedulerService>(
        *plugin_manager_,
        Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(ipc_sp.Get()));
    ASSERT_EQ(scheduler2->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler2->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        1780279200);
}

// ============================================================
// Concurrent safety
// ============================================================

TEST_F(SchedulerServiceTest, ConcurrentEnableDisable)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    constexpr int            num_threads = 8;
    std::vector<std::thread> threads;
    std::atomic<int>         error_count{0};

    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(
            [this, i, &error_count]()
            {
                if (i % 2 == 0)
                {
                    auto result = scheduler_->Enable();
                    if (result != DAS_S_OK && result != DAS_E_FAIL
                        && result != DAS_E_OBJECT_NOT_INIT)
                    {
                        ++error_count;
                    }
                }
                else
                {
                    auto result = scheduler_->Disable();
                    if (result != DAS_S_OK && result != DAS_E_FAIL)
                    {
                        ++error_count;
                    }
                }
            });
    }

    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(error_count, 0);

    auto final_state = scheduler_->Status();
    EXPECT_TRUE(
        final_state == SchedulerState::Stopped
        || final_state == SchedulerState::Running);
}

// ============================================================
// Fake IDasTask for execution tests
// ============================================================

// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
static constexpr DasGuid FakeTaskGuid = {
    0xA1B2C3D4,
    0xE5F6,
    0x7890,
    {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x90}};

class FakeTask final : public Das::PluginInterface::IDasTask
{
public:
    std::atomic<uint32_t>                       ref_count_{0};
    std::atomic<int>                            do_call_count{0};
    std::atomic<bool>                           stop_token_was_null{true};
    std::atomic<bool>                           env_was_null{true};
    std::atomic<bool>                           props_was_null{true};
    std::string                                 last_props_key1_value;
    DasPtr<Das::PluginInterface::IDasStopToken> last_stop_token;

    Das::ExportInterface::DasDate next_date{};
    bool                          has_next_date = false;

    // 异常注入开关：调度器对插件交互段的防护必须在异常抛出时仍走完
    // tick（清理段恒可达）。GetNextExecutionTime 抛非 std::exception 类型
    // 以覆盖兜底 catch (...) 层。
    bool throw_on_do = false;
    bool throw_on_get_next_execution_time = false;

    // Do 返回值注入：默认成功；置失败码模拟任务执行失败（OnTick 失败段
    // focus toast 发射链路的触发条件）。
    DasResult do_result = DAS_S_OK;

    uint32_t AddRef() override { return ++ref_count_; }

    uint32_t Release() override
    {
        auto c = --ref_count_;
        if (c == 0)
        {
            delete this;
        }
        return c;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp) override
    {
        if (!pp)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (iid == DasIidOf<IDasBase>())
        {
            *pp = static_cast<IDasBase*>(this);
            AddRef();
            return DAS_S_OK;
        }
        if (iid == DasIidOf<IDasTypeInfo>())
        {
            *pp = static_cast<IDasTypeInfo*>(this);
            AddRef();
            return DAS_S_OK;
        }
        if (iid == DasIidOf<Das::PluginInterface::IDasTask>())
        {
            *pp = static_cast<Das::PluginInterface::IDasTask*>(this);
            AddRef();
            return DAS_S_OK;
        }
        *pp = nullptr;
        return DAS_E_NO_INTERFACE;
    }

    DasResult GetGuid(DasGuid* p_out_guid) override
    {
        if (!p_out_guid)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = FakeTaskGuid;
        return DAS_S_OK;
    }

    DasResult GetRuntimeClassName(IDasReadOnlyString** pp) override
    {
        if (!pp)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8("FakeTask", pp);
    }

    DasResult Do(
        Das::PluginInterface::IDasStopToken* stop_token,
        Das::ExportInterface::IDasJson*      p_environment_json,
        Das::ExportInterface::IDasJson*      p_task_settings_json) override
    {
        ++do_call_count;
        if (throw_on_do)
        {
            throw std::runtime_error("FakeTask Do injected failure");
        }
        stop_token_was_null = (stop_token == nullptr);
        env_was_null = (p_environment_json == nullptr);
        props_was_null = (p_task_settings_json == nullptr);
        last_stop_token = stop_token;

        if (p_task_settings_json != nullptr)
        {
            DasPtr<IDasReadOnlyString> key;
            auto cr = CreateIDasReadOnlyStringFromUtf8("key1", key.Put());
            if (DAS_S_OK == cr && key)
            {
                DasPtr<IDasReadOnlyString> value;
                if (DAS_S_OK
                        == p_task_settings_json->GetStringByName(
                            key.Get(),
                            value.Put())
                    && value)
                {
                    const char* c_str = nullptr;
                    if (DAS_S_OK == value->GetUtf8(&c_str) && c_str)
                    {
                        last_props_key1_value = c_str;
                    }
                }
            }
        }

        return do_result;
    }

    DasResult GetNextExecutionTime(
        Das::ExportInterface::IDasJson* /*p_task_settings_json*/,
        Das::ExportInterface::DasDate* p_out_date) override
    {
        if (throw_on_get_next_execution_time)
        {
            // 非 std::exception 类型：验证兜底 catch (...) 层。
            throw 42;
        }
        if (!p_out_date)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (has_next_date)
        {
            *p_out_date = next_date;
            return DAS_S_OK;
        }
        return DAS_E_FAIL;
    }
};

// ============================================================
// Fake focus recorder / error lens for the OnTick Do-failure
// emission chain
// ============================================================

// 经接口 getter 读全字段（recorder 侧读 entry 的规范形态，跨进程 proxy
// 也走 getter），捕获 Record 到的条目供断言。
struct RecordedLogEntry
{
    Das::ExportInterface::DasLogType                 type{};
    std::vector<std::string>                         texts;
    std::vector<std::pair<std::string, std::string>> fields;
};

class FakeFocusLogRecorder final
    : public Das::ExportInterface::DasProcessLogRecorderImplBase<
          FakeFocusLogRecorder>
{
public:
    DasResult Record(Das::ExportInterface::IDasLogEntry* entry) override
    {
        if (entry == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        RecordedLogEntry recorded;
        (void)entry->GetType(&recorded.type);
        size_t text_count = 0;
        if (DAS::IsOk(entry->GetTextCount(&text_count)))
        {
            for (size_t index = 0; index < text_count; ++index)
            {
                DasPtr<IDasReadOnlyString> text;
                if (DAS::IsOk(entry->GetText(index, text.Put())) && text)
                {
                    const char* utf8 = nullptr;
                    if (DAS::IsOk(text->GetUtf8(&utf8)) && utf8 != nullptr)
                    {
                        recorded.texts.emplace_back(utf8);
                    }
                }
            }
        }
        size_t field_count = 0;
        if (DAS::IsOk(entry->GetFieldCount(&field_count)))
        {
            for (size_t index = 0; index < field_count; ++index)
            {
                DasPtr<IDasReadOnlyString> key;
                DasPtr<IDasReadOnlyString> value;
                if (DAS::IsOk(entry->GetField(index, key.Put(), value.Put()))
                    && key && value)
                {
                    const char* key_utf8 = nullptr;
                    const char* value_utf8 = nullptr;
                    if (DAS::IsOk(key->GetUtf8(&key_utf8))
                        && DAS::IsOk(value->GetUtf8(&value_utf8))
                        && key_utf8 != nullptr && value_utf8 != nullptr)
                    {
                        recorded.fields.emplace_back(key_utf8, value_utf8);
                    }
                }
            }
        }
        {
            const std::scoped_lock lock(mutex);
            entries.push_back(std::move(recorded));
        }
        return DAS_S_OK;
    }

    std::mutex                    mutex;
    std::vector<RecordedLogEntry> entries;
};

// 只解释单一错误码的 fake lens：命中返回固定文案，其余码 miss——驱动
// ErrorLens 扫描 API 在 OnTick 失败段命中人话的链路。
class TaskFailureCodeLens final
    : public Das::PluginInterface::DasErrorLensImplBase<TaskFailureCodeLens>
{
public:
    DasResult            served_code = DAS_E_FAIL;
    std::string          message;
    std::vector<DasGuid> supported_iids;

    DasResult GetSupportedIids(
        Das::ExportInterface::IDasReadOnlyGuidVector** pp_out_iids) override
    {
        DasPtr<Das::ExportInterface::IDasGuidVector> writable_iids;
        const auto create_result = ::CreateIDasGuidVector(
            supported_iids.data(),
            supported_iids.size(),
            writable_iids.Put());
        if (DAS::IsFailed(create_result))
        {
            return create_result;
        }
        return writable_iids->ToConst(pp_out_iids);
    }

    DasResult GetErrorMessage(
        IDasReadOnlyString*,
        DasResult            error_code,
        IDasReadOnlyString** out_string) override
    {
        if (out_string != nullptr)
        {
            *out_string = nullptr;
        }
        if (error_code != served_code)
        {
            return DAS_E_OUT_OF_RANGE;
        }
        return ::CreateIDasReadOnlyStringFromUtf8(message.c_str(), out_string);
    }
};

// Factory 系列 guid 常量、FactoryTaskSharedState 及所有 Factory* 实现类已迁移至
// 真实 InProcess 测试插件 SchedulerTestPlugin.dll。exe 与 dll 通过共享头
// SchedulerTestSharedState.h 共享同一组 guid 与 FactoryTaskSharedState 布局，
// dll 内对象通过裸指针（DasTestPlugin_SetSharedState 注入）观察 exe
// 持有的状态。
#include <das/Plugins/SchedulerTestPlugin/SchedulerTestSharedState.h>

constexpr char MissingPluginGuidString[] =
    "99999999-0000-4000-8000-000000000001";
constexpr char BannedPluginGuidString[] =
    "99999999-0000-4000-8000-000000000002";
constexpr char LoadFailedPluginGuidString[] =
    "99999999-0000-4000-8000-000000000003";
constexpr char MissingTaskTypeGuidString[] =
    "99999999-0000-4000-8000-000000000004";

void WriteFactoryPluginManifest(
    const std::filesystem::path& manifest_path,
    std::string_view             plugin_guid = FactoryPluginGuidString,
    std::string_view             task_guid = FactoryTaskGuidString,
    bool                         include_authoring = true,
    std::string_view             language = "Cpp",
    std::string_view             execution_component_guid = {},
    bool                         include_flow_components = true)
{
    yyjson::value manifest(Das::Utils::MakeYyjsonObject());
    (*manifest.as_object())[std::string_view("name")] = "FactoryPlugin";
    (*manifest.as_object())[std::string_view("author")] = "Tests";
    (*manifest.as_object())[std::string_view("version")] = "1.0";
    (*manifest.as_object())[std::string_view("guid")] =
        std::string(plugin_guid);
    (*manifest.as_object())[std::string_view("description")] =
        "Factory-backed scheduler test plugin";
    (*manifest.as_object())[std::string_view("supportedSystem")] = "Windows";
    (*manifest.as_object())[std::string_view("language")] =
        std::string(language);
    // IPC 模式：避免 InProcess 加载时 exe 与 dll 各持一份 DasCore 全局状态
    // 导致 MSVC SEH 0xc0000005。插件在 DasHost.exe 子进程中加载，
    // 经 boost::interprocess 共享内存与测试 exe 同步 FactoryTaskSharedState。
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

        yyjson::value components(Das::Utils::MakeYyjsonObject());
        auto          add_component = [&components](
                                          std::string_view component_guid,
                                          std::string_view kind)
        {
            yyjson::value definition(Das::Utils::MakeYyjsonObject());
            (*definition.as_object())[std::string_view("schemaVersion")] = 1;
            (*definition.as_object())[std::string_view("kind")] =
                std::string(kind);
            (*definition.as_object())[std::string_view("componentGuid")] =
                std::string(component_guid);
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
            (*components.as_object())[component_guid] = std::move(entry);
        };
        if (include_flow_components)
        {
            add_component(
                "68F10001-0000-4000-8000-000000000001",
                "das.flow.branch");
            add_component(
                "68F10002-0000-4000-8000-000000000002",
                "das.flow.sequence");
            add_component(
                "68F10003-0000-4000-8000-000000000003",
                "das.flow.delay");
            add_component(
                "68F10004-0000-4000-8000-000000000004",
                "das.flow.for");
            add_component(
                "68F10005-0000-4000-8000-000000000005",
                "das.flow.while");
            add_component(
                "68F10006-0000-4000-8000-000000000006",
                "das.flow.goto");
        }
        else
        {
            // Without the flow-component set the manifest still needs one
            // fake-backed component: the flow guids belong to the real
            // flow-control plugin deployed alongside this variant.
            add_component(FactoryTaskComponentImplGuidString, "test.execution");
        }

        (*task_components.as_object())[std::string_view("components")] =
            std::move(components);
        (*manifest.as_object())[std::string_view("taskComponents")] =
            std::move(task_components);
    }
    {
        yyjson::value task_entry(Das::Utils::MakeYyjsonObject());
        (*task_entry.as_object())[std::string_view("pluginGuid")] =
            std::string(plugin_guid);
        (*task_entry.as_object())[std::string_view("name")] = "factoryTask";
        (*task_entry.as_object())[std::string_view("description")] =
            "Scheduler test task";
        {
            yyjson::value descriptors(Das::Utils::MakeYyjsonArray());
            {
                yyjson::value desc(Das::Utils::MakeYyjsonObject());
                (*desc.as_object())[std::string_view("name")] = "key1";
                (*desc.as_object())[std::string_view("type")] =
                    static_cast<int64_t>(Das::ExportInterface::DAS_TYPE_STRING);
                (*desc.as_object())[std::string_view("defaultValue")] =
                    "default";
                descriptors.as_array()->emplace_back(std::move(desc));
            }
            {
                yyjson::value desc(Das::Utils::MakeYyjsonObject());
                (*desc.as_object())[std::string_view("name")] = "retryCount";
                (*desc.as_object())[std::string_view("type")] =
                    static_cast<int64_t>(Das::ExportInterface::DAS_TYPE_INT);
                (*desc.as_object())[std::string_view("defaultValue")] = 3;
                descriptors.as_array()->emplace_back(std::move(desc));
            }
            (*task_entry.as_object())[std::string_view("descriptors")] =
                std::move(descriptors);
        }
        if (include_authoring)
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
        if (!execution_component_guid.empty())
        {
            yyjson::value execution_component(Das::Utils::MakeYyjsonObject());
            (*execution_component
                  .as_object())[std::string_view("componentGuid")] =
                std::string(execution_component_guid);
            (*task_entry.as_object())[std::string_view("executionComponent")] =
                std::move(execution_component);
        }
        yyjson::value tasks_obj(Das::Utils::MakeYyjsonObject());
        (*tasks_obj.as_object())[std::string_view(task_guid)] =
            std::move(task_entry);
        (*manifest.as_object())[std::string_view("tasks")] =
            std::move(tasks_obj);
    }

    std::ofstream ofs(manifest_path);
    ofs << *Das::Utils::SerializeYyjsonValue(manifest, true);
}

// 在既有工厂清单的目标组件 definition 上覆写端口声明（inputs/outputs/
// signals 探针）——避免为单一用例复制整套清单构造。definition 其余字段
// （kind/componentGuid/config 等）保持 WriteFactoryPluginManifest 原样；
// 探针片段对象的每个键整体替换 definition 的同名键。
void InjectPortDeclarationsIntoFactoryManifest(
    const std::filesystem::path& manifest_path,
    std::string_view             component_guid,
    std::string_view             port_declarations_json)
{
    std::ifstream     ifs(manifest_path);
    const std::string manifest_text(
        (std::istreambuf_iterator<char>(ifs)),
        std::istreambuf_iterator<char>());
    auto parsed = Das::Utils::ParseYyjsonFromString(manifest_text);
    ASSERT_TRUE(parsed.has_value());
    auto manifest_obj = parsed->as_object();
    ASSERT_TRUE(manifest_obj.has_value());
    auto task_components =
        (*manifest_obj)[std::string_view("taskComponents")].as_object();
    ASSERT_TRUE(task_components.has_value());
    auto components =
        (*task_components)[std::string_view("components")].as_object();
    ASSERT_TRUE(components.has_value());
    ASSERT_TRUE(components->contains(component_guid));
    auto entry = (*components)[std::string(component_guid)].as_object();
    ASSERT_TRUE(entry.has_value());
    auto definition = (*entry)[std::string_view("definition")].as_object();
    ASSERT_TRUE(definition.has_value());

    auto fragment = Das::Utils::ParseYyjsonFromString(port_declarations_json);
    ASSERT_TRUE(fragment.has_value());
    auto fragment_obj = fragment->as_object();
    ASSERT_TRUE(fragment_obj.has_value());
    for (const auto& [key, value] : *fragment_obj)
    {
        (*definition)[key] = Das::Utils::CloneYyjsonValue(value);
    }

    std::ofstream ofs(manifest_path, std::ios::trunc);
    ofs << Das::Utils::SerializeYyjsonValue(*parsed).value();
}

// 在既有工厂清单上补声明执行组件并把任务类型的 executionComponent 指到
// 它（manifest 校验要求 executionComponent.componentGuid 必须在
// taskComponents.components 中声明）——照 InjectPortDeclarations 的读改写
// 形态，避免为单一用例复制整套清单构造。
void DeclareExecutionComponentInFactoryManifest(
    const std::filesystem::path& manifest_path,
    std::string_view             task_guid,
    std::string_view             component_guid)
{
    std::ifstream     ifs(manifest_path);
    const std::string manifest_text(
        (std::istreambuf_iterator<char>(ifs)),
        std::istreambuf_iterator<char>());
    auto parsed = Das::Utils::ParseYyjsonFromString(manifest_text);
    ASSERT_TRUE(parsed.has_value());
    auto manifest_obj = parsed->as_object();
    ASSERT_TRUE(manifest_obj.has_value());
    auto task_components =
        (*manifest_obj)[std::string_view("taskComponents")].as_object();
    ASSERT_TRUE(task_components.has_value());
    auto components =
        (*task_components)[std::string_view("components")].as_object();
    ASSERT_TRUE(components.has_value());

    // components 补声明（definition 最小形状与 WriteFactoryPluginManifest
    // 的 add_component 相同）。
    {
        yyjson::value definition(Das::Utils::MakeYyjsonObject());
        (*definition.as_object())[std::string_view("schemaVersion")] = 1;
        (*definition.as_object())[std::string_view("kind")] = "test.execution";
        (*definition.as_object())[std::string_view("componentGuid")] =
            std::string(component_guid);
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
        (*components)[std::string(component_guid)] = std::move(entry);
    }

    // 任务类型的 executionComponent 指到补声明的组件。
    auto tasks = (*manifest_obj)[std::string_view("tasks")].as_object();
    ASSERT_TRUE(tasks.has_value());
    ASSERT_TRUE(tasks->contains(task_guid));
    auto task_entry = (*tasks)[std::string(task_guid)].as_object();
    ASSERT_TRUE(task_entry.has_value());
    yyjson::value execution_component(Das::Utils::MakeYyjsonObject());
    (*execution_component.as_object())[std::string_view("componentGuid")] =
        std::string(component_guid);
    (*task_entry)[std::string_view("executionComponent")] =
        std::move(execution_component);

    std::ofstream ofs(manifest_path, std::ios::trunc);
    ofs << Das::Utils::SerializeYyjsonValue(*parsed).value();
}

// 测试内切换进程级 GlobalLocale（经 C API 构造只读字符串后 Set）；
// 原值的保存与还原由 SchedulerRuntimeBackedTest 夹具负责。
void SetDefaultLocaleForTest(std::string_view locale_utf8)
{
    DAS::DasPtr<IDasReadOnlyString> p_locale;
    ASSERT_EQ(
        ::CreateIDasReadOnlyStringFromUtf8(
            std::string(locale_utf8).c_str(),
            p_locale.Put()),
        DAS_S_OK);
    ASSERT_EQ(::DasSetDefaultLocale(p_locale.Get()), DAS_S_OK);
}

namespace
{

    FakeTask* SetupSchedulerWithFakeTask(
        SchedulerService&                                       scheduler,
        Das::Core::SettingsManager::SettingsManager&            sm,
        const std::filesystem::path&                            plugin_dir,
        Das::Core::ForeignInterfaceHost::TestablePluginManager& pm)
    {
        // Create a fake task and inject it into PluginManager's features
        auto* fake_task = new FakeTask();
        fake_task->AddRef();

        // Register as a feature so GetFeaturesByType returns it
        pm.RegisterTestFeature(
            Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK,
            {},
            static_cast<IDasBase*>(fake_task));

        // Write one task instance in the scheduler state
        sm.CreateProfile("0");
        yyjson::value task0(Das::Utils::MakeYyjsonObject());
        (*task0.as_object())[std::string_view("id")] = 0;
        (*task0.as_object())[std::string_view("taskGuid")] =
            "A1B2C3D4-E5F6-7890-ABCD-EF1234567890";
        (*task0.as_object())[std::string_view("pluginGuid")] =
            "00000000-0000-0000-0000-000000000000";
        (*task0.as_object())[std::string_view("nextExecutionTime")] =
            yyjson::value{};
        {
            yyjson::value p(Das::Utils::MakeYyjsonObject());
            (*p.as_object())[std::string_view("key1")] = "value1";
            (*task0.as_object())[std::string_view("properties")] = std::move(p);
        }
        WriteSchedulerState(sm, 1, {0}, {task0});

        auto init_result = scheduler.Initialize(plugin_dir, {});
        EXPECT_EQ(init_result, DAS_S_OK) << "Initialize should succeed";

        return fake_task;
    }

} // namespace

// ============================================================
// Execution test fixture (with IO thread for timer)
// ============================================================

class SchedulerExecutionTest : public ::testing::Test
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

        settings_manager_ =
            std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContext(false);
        plugin_manager_ = std::make_unique<
            Das::Core::ForeignInterfaceHost::TestablePluginManager>(
            *settings_manager_,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_.Get()));
        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_.Get()));

        // Start IO thread so that steady_timer can fire
        io_thread_ = std::thread(
            [this]()
            {
                auto& io = ipc_sp_->GetIoContext();
                const boost::asio::executor_work_guard<
                    boost::asio::io_context::executor_type>
                    work(io.get_executor());
                io.run();
            });
    }

    void TearDown() override
    {
        ipc_sp_->GetIoContext().stop();
        if (io_thread_.joinable())
        {
            io_thread_.join();
        }
        scheduler_.reset();
        plugin_manager_.reset();
        settings_manager_.reset();
        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path test_dir_;
    std::filesystem::path settings_dir_;
    std::filesystem::path plugin_dir_;
    std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
                                                          settings_manager_;
    DAS::DasPtr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
    std::unique_ptr<Das::Core::ForeignInterfaceHost::TestablePluginManager>
                                      plugin_manager_;
    std::unique_ptr<SchedulerService> scheduler_;
    std::thread                       io_thread_;
};

class SchedulerRuntimeBackedTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 进程级 GlobalLocale 是跨测试共享状态：改 locale 的用例会污染同
        // 进程后续用例。SetUp 保存、TearDown 还原，保证直接跑整二进制
        // （非 ctest 分进程）时后续用例不被污染。
        ::DasGetDefaultLocale(saved_locale_.Put());

        test_dir_ = UniqueTestDir();
        std::filesystem::create_directories(test_dir_);
        settings_dir_ = test_dir_ / DAS_PATH_LITERAL("settings");
        std::filesystem::create_directories(settings_dir_);
        plugin_dir_ = test_dir_ / DAS_PATH_LITERAL("plugins");
        std::filesystem::create_directories(plugin_dir_);
        manifest_path_ = plugin_dir_ / DAS_PATH_LITERAL("FactoryPlugin.json");
        WriteFactoryPluginManifest(manifest_path_);

        // 先创建共享内存段并构造 FactoryTaskSharedState，
        // 再设置 DAS_SCHEDULER_TEST_SHM_NAME 环境变量（被 DasHost.exe 子进程
        // 通过 DasTestPlugin_SetSharedMemoryName 回退读取），
        // 最后部署 dll。IPC 模式下 PluginManager spawn 的 DasHost.exe
        // 会在 DasCoCreatePlugin 中读环境变量打开段，取回同一份 state。
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
        // IPC 模式下 PluginManager 需要 DasHost.exe 路径才能 spawn 子进程
        // 加载 FactoryPlugin.dll。DAS_HOST_EXE_PATH 由 ctest 注入。
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
            // host 未配置时 PluginManager.Initialize 会因无法 spawn 而失败，
            // 测试断言会暴露具体错误，这里不提前 fail。
        }

        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_.Get()));

        ASSERT_EQ(plugin_manager_->Initialize(1), DAS_S_OK);

        // IpcContext::Run() 内部调用 IpcRunLoop::Run()，后者将 running_ 置 true
        // 再阻塞在 io_context->run()。InternalHostReceiveLoop 的循环条件是
        // while (running_.load())，若直接 io.run() 而不经 Run()，running_ 始终
        // 为 false，接收协程刚 spawn 即退出，IPC 响应永远收不到。
        io_thread_ = std::thread([this]() { ipc_sp_->Run(); });
    }

    // 创建以 PID + 全局递增计数器命名的 managed_shared_memory 段，
    // 并把 FactoryTaskSharedState POD 构造在段内（key 固定为 "state"）。
    // 段名写入 DAS_SCHEDULER_TEST_SHM_NAME 环境变量，DasHost.exe 子进程
    // 在 DasCoCreatePlugin 中通过 DasTestPlugin_SetSharedMemoryName
    // 打开同一段。
    void CreateSharedStateSegment()
    {
        const uint64_t pid =
#ifdef _WIN32
            static_cast<uint64_t>(::GetCurrentProcessId());
#else
            static_cast<uint64_t>(::getpid());
#endif
        const uint64_t counter = s_shm_counter.fetch_add(1);
        shm_name_ = "DasSchedulerTest_" + std::to_string(pid) + "_"
                    + std::to_string(counter);

        // 段名全局唯一，但仍 remove 以防残留（极端竞态）。
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

    // 部署真实 SchedulerTestPlugin：从构建目录加载由 CMake 提供的实际产物文件名
    // （DAS_SCHEDULER_TEST_PLUGIN_FILENAME，跨平台），以 FactoryPlugin.dll
    // 之名 拷贝到 plugin_dir_，使 WriteFactoryPluginManifest 生成的
    // FactoryPlugin.json (name=FactoryPlugin) 能找到该 dll。
    // IPC 模式下 PluginManager 通过 DasHost.exe 子进程加载该 dll，
    // 不再需要 LoadLibrary 注入裸指针。
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

    // 部署构建产物里的真实插件（dll + json 成对拷贝，json 文件名带平台前缀
    // 差异：dll 由 manifest name 字段定位，json 文件名不必与 name 一致）。
    // source_subdir 非空时源产物取自构建树目录式插件子目录（dll 与 json 同
    // 子目录），目标仍平铺拷进 plugin_dir_——manifest 的 name 字段按文件名
    // 找同目录 dll，加载不受布局影响。force_ipc 为 true 时把部署副本的
    // loadMode 补写为 ipc——测试 exe 静态链接 DasCore，进程内再加载插件
    // dll 会产生双份 DasCore 全局状态。
    void DeployRealPluginArtifacts(
        const std::string& dll_name,
        const std::string& manifest_name,
        bool               force_ipc,
        const std::string& source_subdir = "")
    {
        namespace fs = std::filesystem;
        const fs::path build_plugin_dir = IpcTestConfig::GetPluginDir();
        const fs::path source_dir =
            source_subdir.empty()
                ? build_plugin_dir
                : build_plugin_dir / Das::Utils::PathFromUtf8(source_subdir);

        const fs::path src_dll =
            source_dir / Das::Utils::PathFromUtf8(dll_name);
        ASSERT_TRUE(fs::exists(src_dll))
            << "plugin dll not found at: " << Das::Utils::PathToUtf8(src_dll);
        fs::copy_file(
            src_dll,
            plugin_dir_ / Das::Utils::PathFromUtf8(dll_name),
            fs::copy_options::overwrite_existing);

        const fs::path src_manifest =
            source_dir / Das::Utils::PathFromUtf8(manifest_name);
        ASSERT_TRUE(fs::exists(src_manifest))
            << "plugin manifest not found at: "
            << Das::Utils::PathToUtf8(src_manifest);
        if (!force_ipc)
        {
            fs::copy_file(
                src_manifest,
                plugin_dir_ / Das::Utils::PathFromUtf8(manifest_name),
                fs::copy_options::overwrite_existing);
            return;
        }

        std::ifstream     ifs(src_manifest);
        const std::string manifest_text(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        auto parsed = Das::Utils::ParseYyjsonFromString(manifest_text);
        ASSERT_TRUE(parsed.has_value());
        auto parsed_obj = parsed->as_object();
        ASSERT_TRUE(parsed_obj.has_value());
        (*parsed_obj)[std::string_view("loadMode")] = "ipc";
        std::ofstream ofs(
            plugin_dir_ / Das::Utils::PathFromUtf8(manifest_name),
            std::ios::trunc);
        ofs << Das::Utils::SerializeYyjsonValue(*parsed).value();
    }

    void TearDown() override
    {
        // 还原进程级 GlobalLocale（须先于一切清理，见 SetUp 注释）。
        if (saved_locale_)
        {
            ::DasSetDefaultLocale(saved_locale_.Get());
        }

        // SetUp 可能在多个早期步骤（CreateSharedStateSegment、
        // DeploySchedulerTestPlugin 的 GetPluginDir/GetDasHostPath 抛
        // std::runtime_error、settings_manager_/ipc_sp_/plugin_manager_/
        // scheduler_ 构造抛 bad_alloc 等）抛异常退出，此时部分成员仍是
        // 默认构造的空指针。gtest 在 --gtest_catch_exceptions=0 下不再吞
        // 异常，TearDown 仍被调用，必须对每个成员独立判空才能避免在半
        // 初始化状态上解引用 nullptr 触发 ASAN access-violation。

        if (scheduler_ && scheduler_->Status() == SchedulerState::Running)
        {
            EXPECT_EQ(scheduler_->Disable(), DAS_S_OK);
        }

        // 释放顺序：先 scheduler_/plugin_manager_(释放 IPC 句柄与子进程
        // 对象) -> 再停事件循环 -> 最后清理共享内存段。PluginManager
        // 关停会把卸载任务派发到 BusinessThread：io_context 先停会让该
        // 任务失去执行载体，非 BT 调用方的泵化等待随之死等（并发下窗口
        // 张开即挂起）——必须在 RequestStop 之前完成 Shutdown。
        scheduler_.reset();
        if (plugin_manager_)
        {
            plugin_manager_->Shutdown();
        }
        plugin_manager_.reset();
        settings_manager_.reset();

        // RequestStop 内部将 running_ 置 false 并 io_context->stop()，
        // 使 Run() 返回，io_thread_ 随后 join。仅当 IpcContext 已成功
        // 创建时才需要通知它退出；SetUp 早期失败时 ipc_sp_ 是 nullptr。
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
        // test_dir_ 内含 FactoryPlugin.dll，可能在 PluginManager/CppHost 的
        // boost::dll 句柄延迟释放或 scheduler 异步路径下仍被映射，
        // Windows 不允许删除被映射文件。用不抛异常的 remove_all 重载，
        // 文件残留不影响测试断言（下一测试用新的 UniqueTestDir）。
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }

    bool WaitForExecutions(
        size_t                    expected_count,
        std::chrono::milliseconds timeout)
    {
        auto            deadline = std::chrono::steady_clock::now() + timeout;
        ipc_scoped_lock lock(shared_state_->mutex);
        return shared_state_->cv.wait_until(
            lock,
            deadline,
            [this, expected_count]()
            { return shared_state_->executed_count >= expected_count; });
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
    // 段内指针：生命周期由 shm_segment_ 管理，不可 free。
    FactoryTaskSharedState* shared_state_ = nullptr;
    std::unique_ptr<boost::interprocess::managed_shared_memory> shm_segment_;
    std::string                                                 shm_name_;
    std::thread                                                 io_thread_;
    // SetUp 保存 / TearDown 还原的进程级 GlobalLocale 原值。
    Das::DasPtr<IDasReadOnlyString> saved_locale_;

    static inline std::atomic<uint64_t> s_shm_counter{0};
};

// ====== host 申报（准入闸门注册，注册器同时装入 listener） ======
// 申报值=本族用例加载的 loadMode:ipc 插件数上界（一插件一 host）：基线
// 部署探针 FactoryPlugin（1 host），真实数据用例再叠加两个真实插件包
// （DasGraphTask/DasFlowControl，共 3）。生产路径 spawn（PluginManager→
// IpcRemotePluginHost）测试侧不可见，故无违约计数（用户裁决：不动生产
// 文件）。插件数变动时同步改申报值。
DAS_TEST_MAX_HOSTS_SUITE(SchedulerRuntimeBackedTest, 3)

// ============================================================
// Execution tests
// ============================================================

TEST_F(SchedulerServiceTest, OnTick_SkipsInvalidInstance)
{
    settings_manager_->CreateProfile("0");

    // Write a corrupt task instance file
    yyjson::value scheduler_index(Das::Utils::MakeYyjsonObject());
    (*scheduler_index.as_object())[std::string_view("nextTaskId")] = 1;
    {
        yyjson::value arr(Das::Utils::MakeYyjsonArray());
        (*arr.as_array()).emplace_back(0);
        (*scheduler_index.as_object())[std::string_view("taskOrder")] =
            std::move(arr);
    }
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    auto task_file = settings_dir_ / DAS_PATH_LITERAL("0")
                     / DAS_PATH_LITERAL("taskId0.json");
    {
        std::ofstream ofs{task_file};
        ofs << "CORRUPT DATA";
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Enable should fail because no available task instances
    auto result = scheduler_->Enable();
    EXPECT_EQ(result, DAS_E_OBJECT_NOT_INIT);
}

TEST_F(SchedulerServiceTest, OnTick_SkipsUnavailableInstance)
{
    settings_manager_->CreateProfile("0");

    // Write a task instance referencing a non-existent task type
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-1111-1111-1111-111111111111";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Enable should fail because no available task instances
    auto result = scheduler_->Enable();
    EXPECT_EQ(result, DAS_E_OBJECT_NOT_INIT);
}

TEST_F(SchedulerExecutionTest, OnTick_ExecutesAvailableInstance)
{
    auto* fake = SetupSchedulerWithFakeTask(
        *scheduler_,
        *settings_manager_,
        plugin_dir_,
        *plugin_manager_);

    // Enable scheduler
    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    // Wait for at least one tick to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Disable
    auto stop_result = scheduler_->Disable();
    EXPECT_EQ(stop_result, DAS_S_OK);

    // Task should have been executed at least once
    EXPECT_GE(fake->do_call_count, 1);

    // Task received non-null stop token
    EXPECT_FALSE(fake->stop_token_was_null);

    // Task received non-null environment and props
    EXPECT_FALSE(fake->env_was_null);
    EXPECT_FALSE(fake->props_was_null);
}

TEST_F(SchedulerExecutionTest, OnTick_ReceivesNonNullInputs)
{
    auto* fake = SetupSchedulerWithFakeTask(
        *scheduler_,
        *settings_manager_,
        plugin_dir_,
        *plugin_manager_);

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    auto stop_result = scheduler_->Disable();
    EXPECT_EQ(stop_result, DAS_S_OK);

    // Verify properties JSON was passed via IDasJson::GetStringByName
    EXPECT_EQ(fake->last_props_key1_value, "value1");
}

TEST_F(SchedulerExecutionTest, OnTick_RefreshesNextExecutionTime)
{
    auto* fake = SetupSchedulerWithFakeTask(
        *scheduler_,
        *settings_manager_,
        plugin_dir_,
        *plugin_manager_);

    // Set up next execution time to return
    fake->has_next_date = true;
    fake->next_date = {2026, 6, 15, 10, 30, 0};

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    auto stop_result = scheduler_->Disable();
    EXPECT_EQ(stop_result, DAS_S_OK);

    // Verify nextExecutionTime was persisted
    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    ASSERT_TRUE((*persisted.as_object())
                    .contains(std::string_view("nextExecutionTime")));
    ASSERT_TRUE((*persisted.as_object())[std::string_view("nextExecutionTime")]
                    .is_sint());
    // 2026-06-15T10:30:00Z = 1781519400
    EXPECT_EQ(
        (*persisted.as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        1781519400);
}

TEST_F(SchedulerExecutionTest, OnTick_GetNextExecutionTimeFailure_SetsSentinel)
{
    auto* fake = SetupSchedulerWithFakeTask(
        *scheduler_,
        *settings_manager_,
        plugin_dir_,
        *plugin_manager_);

    // GetNextExecutionTime returns failure (has_next_date = false by default)
    fake->has_next_date = false;

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    // Wait for one tick to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    auto stop_result = scheduler_->Disable();
    EXPECT_EQ(stop_result, DAS_S_OK);

    // Task should have been executed at least once
    EXPECT_GE(fake->do_call_count, 1);

    // The sentinel far-future timestamp should be persisted
    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    ASSERT_TRUE((*persisted.as_object())
                    .contains(std::string_view("nextExecutionTime")));
    ASSERT_TRUE((*persisted.as_object())[std::string_view("nextExecutionTime")]
                    .is_sint());
    EXPECT_EQ(
        (*persisted.as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        4102444799LL);
}

// 插件 Do 抛异常：tick 必须完整走完（执行槽清理、收尾状态机可达），
// 调度器不得永久停留在执行中状态。
TEST_F(
    SchedulerExecutionTest,
    OnTick_DoThrowsException_CompletesTickAndClearsCurrentTask)
{
    auto* fake = SetupSchedulerWithFakeTask(
        *scheduler_,
        *settings_manager_,
        plugin_dir_,
        *plugin_manager_);

    fake->throw_on_do = true;

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    // Wait for one tick to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // 执行槽被清理后 Disable 才能走到 Stopped——异常穿透路径会卡住。
    auto stop_result = scheduler_->Disable();
    EXPECT_EQ(stop_result, DAS_S_OK);
    ASSERT_TRUE(
        WaitForSchedulerStopped(*scheduler_, std::chrono::milliseconds(2000)));

    EXPECT_GE(fake->do_call_count, 1);

    // tick 走过 Do 调用点：GetNextExecutionTime 返回失败（fake 默认），
    // 哨兵时间落盘证明刷新与持久化段照常执行。
    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    ASSERT_TRUE((*persisted.as_object())
                    .contains(std::string_view("nextExecutionTime")));
    ASSERT_TRUE((*persisted.as_object())[std::string_view("nextExecutionTime")]
                    .is_sint());
    EXPECT_EQ(
        (*persisted.as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        4102444799LL);
}

// 插件 GetNextExecutionTime 抛异常：与返回错误码同型，哨兵时间落盘，
// tick 完整走完。
TEST_F(
    SchedulerExecutionTest,
    OnTick_GetNextExecutionTimeThrowsException_SetsSentinel)
{
    auto* fake = SetupSchedulerWithFakeTask(
        *scheduler_,
        *settings_manager_,
        plugin_dir_,
        *plugin_manager_);

    fake->throw_on_get_next_execution_time = true;

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    // Wait for one tick to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    auto stop_result = scheduler_->Disable();
    EXPECT_EQ(stop_result, DAS_S_OK);
    ASSERT_TRUE(
        WaitForSchedulerStopped(*scheduler_, std::chrono::milliseconds(2000)));

    EXPECT_GE(fake->do_call_count, 1);

    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    ASSERT_TRUE((*persisted.as_object())
                    .contains(std::string_view("nextExecutionTime")));
    ASSERT_TRUE((*persisted.as_object())[std::string_view("nextExecutionTime")]
                    .is_sint());
    EXPECT_EQ(
        (*persisted.as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        4102444799LL);
}

// 任务 Do 返回失败：OnTick 失败段经 C ABI 按名解析 recorder（fake 注册为
// das.focus-log-recorder），发射一条 focus 条目——错误说明文本来自 fake
// lens（经 ErrorLens 扫描按码命中）并附 task id，displayChannels 含
// toast+log 双渠道。
TEST_F(SchedulerExecutionTest, OnTick_DoFailureEmitsFocusToastEntry)
{
    auto* fake = SetupSchedulerWithFakeTask(
        *scheduler_,
        *settings_manager_,
        plugin_dir_,
        *plugin_manager_);
    fake->do_result = DAS_E_FAIL;

    // fake lens 注册进进程 active ErrorLensManager：本 fixture 不走
    // Initialize 的测试须显式 Initialize 才会置 active（析构路径会清）。
    ASSERT_EQ(plugin_manager_->Initialize(1), DAS_S_OK);
    auto* lens = TaskFailureCodeLens::MakeRaw();
    lens->served_code = DAS_E_FAIL;
    lens->message = "fake lens failure explanation";
    lens->supported_iids = {DasGuid{
        0x5C6D7E8F,
        0x9A0B,
        0x4C1D,
        {0x80, 0x2E, 0x3F, 0x44, 0x55, 0x66, 0x77, 0x88}}};
    {
        DasPtr<Das::ExportInterface::IDasGuidVector> writable_iids;
        ASSERT_EQ(
            CreateIDasGuidVector(
                lens->supported_iids.data(),
                lens->supported_iids.size(),
                writable_iids.Put()),
            DAS_S_OK);
        DasPtr<Das::ExportInterface::IDasReadOnlyGuidVector> readonly_iids;
        ASSERT_EQ(writable_iids->ToConst(readonly_iids.Put()), DAS_S_OK);
        auto* active_lens_manager =
            Das::Core::ForeignInterfaceHost::GetActiveErrorLensManager();
        ASSERT_NE(active_lens_manager, nullptr);
        ASSERT_EQ(
            active_lens_manager->Register(readonly_iids.Get(), lens),
            DAS_S_OK);
    }

    // fake recorder 注册为 das.focus-log-recorder（OnTick 在 BT 线程经
    // C ABI 按名解析到它，in-process 快路径）。MakeRaw 引用归本测试持有，
    // 注销后 Release 配对。
    auto* recorder = FakeFocusLogRecorder::MakeRaw();
    ASSERT_EQ(
        ipc_sp_->RegisterServiceByName(
            recorder,
            DasIidOf<Das::ExportInterface::IDasProcessLogRecorder>(),
            "das.focus-log-recorder"),
        DAS_S_OK);

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    auto stop_result = scheduler_->Disable();
    EXPECT_EQ(stop_result, DAS_S_OK);
    ASSERT_TRUE(
        WaitForSchedulerStopped(*scheduler_, std::chrono::milliseconds(2000)));

    EXPECT_GE(fake->do_call_count, 1);

    {
        const std::scoped_lock lock(recorder->mutex);
        ASSERT_EQ(recorder->entries.size(), 1u);
        const auto& entry = recorder->entries[0];
        EXPECT_EQ(entry.type, Das::ExportInterface::DAS_LOG_TYPE_ERROR);
        ASSERT_EQ(entry.texts.size(), 1u);
        EXPECT_NE(
            entry.texts[0].find("fake lens failure explanation"),
            std::string::npos);
        EXPECT_NE(entry.texts[0].find("task 0"), std::string::npos);
        bool channels_found = false;
        for (const auto& [key, value] : entry.fields)
        {
            if (key == std::string("displayChannels"))
            {
                channels_found = true;
                EXPECT_NE(value.find("toast"), std::string::npos);
                EXPECT_NE(value.find("log"), std::string::npos);
            }
        }
        EXPECT_TRUE(channels_found) << "displayChannels field missing";
    }

    ipc_sp_->UnregisterServiceByName("das.focus-log-recorder");
    recorder->Release();
    lens->Release();
}

// 任务 Do 成功：零 focus 条目（发射只在失败时发生，非每 tick）。
TEST_F(SchedulerExecutionTest, OnTick_DoSuccessEmitsNoFocusEntry)
{
    auto* fake = SetupSchedulerWithFakeTask(
        *scheduler_,
        *settings_manager_,
        plugin_dir_,
        *plugin_manager_);

    auto* recorder = FakeFocusLogRecorder::MakeRaw();
    ASSERT_EQ(
        ipc_sp_->RegisterServiceByName(
            recorder,
            DasIidOf<Das::ExportInterface::IDasProcessLogRecorder>(),
            "das.focus-log-recorder"),
        DAS_S_OK);

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    auto stop_result = scheduler_->Disable();
    EXPECT_EQ(stop_result, DAS_S_OK);
    ASSERT_TRUE(
        WaitForSchedulerStopped(*scheduler_, std::chrono::milliseconds(2000)));

    EXPECT_GE(fake->do_call_count, 1);

    {
        const std::scoped_lock lock(recorder->mutex);
        EXPECT_EQ(recorder->entries.size(), 0u);
    }

    ipc_sp_->UnregisterServiceByName("das.focus-log-recorder");
    recorder->Release();
}

TEST_F(SchedulerExecutionTest, Stop_RequestsCancellationToken)
{
    auto* fake = SetupSchedulerWithFakeTask(
        *scheduler_,
        *settings_manager_,
        plugin_dir_,
        *plugin_manager_);

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    // Let at least one tick happen
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Stop should complete successfully
    auto result = scheduler_->Disable();
    EXPECT_EQ(result, DAS_S_OK);

    // Disable is asynchronous: wait for the stop thread to finish before
    // checking the cancellation token state.
    ASSERT_TRUE(
        WaitForSchedulerStopped(*scheduler_, std::chrono::milliseconds(2000)));
    ASSERT_FALSE(fake->stop_token_was_null);
    ASSERT_TRUE(fake->last_stop_token);

    bool stop_requested = false;
    ASSERT_EQ(fake->last_stop_token->StopRequested(&stop_requested), DAS_S_OK);
    EXPECT_TRUE(stop_requested);
}

TEST_F(SchedulerExecutionTest, OnTick_DoesNotHoldMutexDuringDo)
{
    // This test verifies that Get() can be called while a task is running.
    // If mutex were held during Do, Get() would deadlock.
    auto* fake = SetupSchedulerWithFakeTask(
        *scheduler_,
        *settings_manager_,
        plugin_dir_,
        *plugin_manager_);

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    // Give the scheduler a moment to start ticking
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Get should not deadlock (it acquires mutex internally)
    auto state = scheduler_->Get();
    EXPECT_TRUE((*state.as_object()).contains(std::string_view("state")));

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    auto stop_result = scheduler_->Disable();
    EXPECT_EQ(stop_result, DAS_S_OK);
}

TEST_F(SchedulerRuntimeBackedTest, Initialize_DiscoversFlatManifestTaskTypes)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("availableTaskTypes")]
            .as_array()
            ->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("availableTaskTypes")]
                .as_array())[0]
              .as_object())[std::string_view("taskGuid")]
            .as_string()
            .value(),
        FactoryTaskGuidString);
    EXPECT_EQ(plugin_manager_->GetLoadedPluginCount(), 1u);
}

TEST_F(SchedulerRuntimeBackedTest, OnTick_RespectsPersistedNextExecutionTime)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    int64_t first_task_id = -1;
    int64_t second_task_id = -1;
    ASSERT_EQ(scheduler_->AddTask(FactoryTaskGuid, &first_task_id), DAS_S_OK);
    ASSERT_EQ(scheduler_->AddTask(FactoryTaskGuid, &second_task_id), DAS_S_OK);

    yyjson::value internal_props(Das::Utils::MakeYyjsonObject());
    // 2099-01-01T00:00:00Z = 4070908800
    (*internal_props.as_object())[std::string_view("nextExecutionTime")] =
        4070908800;
    ASSERT_EQ(
        scheduler_->UpdateTaskInternalProperties(first_task_id, internal_props),
        DAS_S_OK);

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    ASSERT_TRUE(WaitForExecutions(1, std::chrono::milliseconds(500)));
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);

    auto first_task_json =
        settings_manager_->GetTaskInstanceJson("0", first_task_id);

    // Disable 返回只保证 persist 事件已入队，入队到 persist 线程刷盘是
    // 异步的——立即断言超出契约边界。带界轮询重读；丢事件场景（null
    // 永不变化）会在超时处失败，不是掩蔽。
    bool second_next_time_persisted = false;
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto second_task_json =
                settings_manager_->GetTaskInstanceJson("0", second_task_id);
            if (!(*second_task_json
                       .as_object())[std::string_view("nextExecutionTime")]
                     .is_null())
            {
                second_next_time_persisted = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    EXPECT_TRUE(second_next_time_persisted);

    // 未执行的任务不受时序影响，保持精确值断言。
    EXPECT_EQ(
        (*first_task_json.as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        4070908800);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    DisableThenDestruct_PersistsLastNextExecutionTime)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    int64_t first_task_id = -1;
    int64_t second_task_id = -1;
    ASSERT_EQ(scheduler_->AddTask(FactoryTaskGuid, &first_task_id), DAS_S_OK);
    ASSERT_EQ(scheduler_->AddTask(FactoryTaskGuid, &second_task_id), DAS_S_OK);

    yyjson::value internal_props(Das::Utils::MakeYyjsonObject());
    // 2099-01-01T00:00:00Z = 4070908800
    (*internal_props.as_object())[std::string_view("nextExecutionTime")] =
        4070908800;
    ASSERT_EQ(
        scheduler_->UpdateTaskInternalProperties(first_task_id, internal_props),
        DAS_S_OK);

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    ASSERT_TRUE(WaitForExecutions(1, std::chrono::milliseconds(500)));

    // Disable 后立即在测试体内析构（TearDown 对空 unique_ptr 判空安全，
    // settings_manager_/plugin_manager_ 均存活）。断言必须放在 reset()
    // 之后：~SchedulerService 的 persist 队列 join 保证 drain-then-exit
    // 全部落盘，这是确定性观测点，不与 persist 线程刷盘时序竞速——
    // 这正是停机竞态的目标场景：尾段 persist 事件若在析构关队列后才
    // 入队即静默丢失，JSON 永远 null，重启后任务多跑一次。
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
    scheduler_.reset();

    auto executed_task_json =
        settings_manager_->GetTaskInstanceJson("0", second_task_id);
    EXPECT_FALSE(
        (*executed_task_json.as_object())[std::string_view("nextExecutionTime")]
            .is_null());

    auto skipped_task_json =
        settings_manager_->GetTaskInstanceJson("0", first_task_id);
    EXPECT_EQ(
        (*skipped_task_json.as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        4070908800);
}

TEST_F(SchedulerRuntimeBackedTest, AddTask_CreatesDistinctRuntimeInstances)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    int64_t first_task_id = -1;
    int64_t second_task_id = -1;
    ASSERT_EQ(scheduler_->AddTask(FactoryTaskGuid, &first_task_id), DAS_S_OK);
    ASSERT_EQ(scheduler_->AddTask(FactoryTaskGuid, &second_task_id), DAS_S_OK);

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    ASSERT_TRUE(WaitForExecutions(2, std::chrono::milliseconds(500)));
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);

    // Allow config persist thread to flush queued events
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto first_task_json =
        settings_manager_->GetTaskInstanceJson("0", first_task_id);
    auto second_task_json =
        settings_manager_->GetTaskInstanceJson("0", second_task_id);

    ASSERT_TRUE(
        (*first_task_json.as_object())[std::string_view("nextExecutionTime")]
            .is_sint());
    ASSERT_TRUE(
        (*second_task_json.as_object())[std::string_view("nextExecutionTime")]
            .is_sint());
    EXPECT_NE(
        (*first_task_json.as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        (*second_task_json.as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value());

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->executed_count, 2u);
    EXPECT_NE(
        shared_state_->executed_instance_ids[0],
        shared_state_->executed_instance_ids[1]);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringTaskCapabilityRegistryGetDocument)
{
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] = FactoryTaskGuidString;
    (*task0.as_object())[std::string_view("pluginGuid")] =
        FactoryPluginGuidString;
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto result = scheduler_->GetTaskAuthoringDocument(0, request);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE(obj->contains(std::string_view("document")));
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    // Phase 84 strict wire: viewKind replaces kind; Core no longer merges a
    // catalog into the Provider document.
    EXPECT_EQ(
        (*document)[std::string_view("viewKind")].as_string().value_or(""),
        std::string_view("formSequence"));
    EXPECT_FALSE(document->contains(std::string_view("catalog")));
    EXPECT_TRUE(obj->contains(std::string_view("diagnostics")));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->authoring_session_count, 1);
    EXPECT_EQ(shared_state_->decoy_authoring_create_count, 0);
    EXPECT_EQ(shared_state_->get_document_count, 1);
    EXPECT_EQ(shared_state_->last_context_revision, 0);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerExecutionComponentCapabilityRegistry)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryExecutionComponentGuidString);

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto execution_component =
        scheduler_->FindTaskExecutionComponent(FactoryTaskGuid);
    ASSERT_TRUE(execution_component.has_value());
    EXPECT_EQ(
        *execution_component,
        Das::Core::ForeignInterfaceHost::MakeDasGuid(
            FactoryExecutionComponentGuidString));
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerExecutionComponentCapabilityMissingReturnsEmpty)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto execution_component =
        scheduler_->FindTaskExecutionComponent(FactoryTaskGuid);
    EXPECT_FALSE(execution_component.has_value());

    int64_t task_id = -1;
    EXPECT_EQ(scheduler_->AddTask(FactoryTaskGuid, &task_id), DAS_S_OK);
    EXPECT_GE(task_id, 0);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    UpdateTaskProperties_AuthoringTask_ReturnsAccessDenied)
{
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] = FactoryTaskGuidString;
    (*task0.as_object())[std::string_view("pluginGuid")] =
        FactoryPluginGuidString;
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    yyjson::value properties(Das::Utils::MakeYyjsonObject());
    (*properties.as_object())[std::string_view("key1")] = "direct";
    EXPECT_EQ(
        scheduler_->UpdateTaskProperties(0, properties),
        DAS_E_ACCESS_DENIED);

    auto state = scheduler_->Get();
    auto tasks = (*state.as_object())[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    auto task_obj = (*tasks)[0].as_object();
    ASSERT_TRUE(task_obj.has_value());
    EXPECT_EQ(
        (*task_obj)[std::string_view("configurationMode")].as_string().value_or(
            ""),
        std::string_view("authoring"));
    EXPECT_FALSE(
        (*task_obj)[std::string_view("propertiesWritable")].as_bool().value_or(
            true));
}

TEST_F(SchedulerRuntimeBackedTest, SchedulerAuthoringRevisionConflict)
{
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] = FactoryTaskGuidString;
    (*task0.as_object())[std::string_view("pluginGuid")] =
        FactoryPluginGuidString;
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    {
        yyjson::value authoring(Das::Utils::MakeYyjsonObject());
        (*authoring.as_object())[std::string_view("revision")] = 3;
        (*task0.as_object())[std::string_view("authoring")] =
            std::move(authoring);
    }
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto change = MakeStrictAuthoringChange(2);
    auto result = scheduler_->ApplyTaskAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("revisionConflict"));
    EXPECT_EQ(
        (*obj)[std::string_view("currentRevision")].as_sint().value_or(-1),
        3);

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 0);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringApplyPersistsSnapshot_AuthoringPersistence_ExecutionSnapshot)
{
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] = FactoryTaskGuidString;
    (*task0.as_object())[std::string_view("pluginGuid")] =
        FactoryPluginGuidString;
    (*task0.as_object())[std::string_view("nextExecutionTime")] =
        yyjson::value{};
    {
        yyjson::value props(Das::Utils::MakeYyjsonObject());
        (*props.as_object())[std::string_view("key1")] = "initial";
        (*task0.as_object())[std::string_view("properties")] = std::move(props);
    }
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto change = MakeStrictAuthoringChange(0);
    auto result = scheduler_->ApplyTaskAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
    EXPECT_TRUE(obj->contains(std::string_view("acceptedProperties")));
    EXPECT_TRUE(obj->contains(std::string_view("document")));
    EXPECT_TRUE(obj->contains(std::string_view("diagnostics")));
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(
        (*document)[std::string_view("viewKind")].as_string().value_or(""),
        std::string_view("formSequence"));
    EXPECT_EQ(
        (*document)[std::string_view("revision")].as_sint().value_or(-1),
        1);

    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    auto props = (*persisted_obj)[std::string_view("properties")].as_object();
    ASSERT_TRUE(props.has_value());
    EXPECT_EQ(
        (*props)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("accepted"));
    auto authoring =
        (*persisted_obj)[std::string_view("authoring")].as_object();
    ASSERT_TRUE(authoring.has_value());
    EXPECT_EQ(
        (*authoring)[std::string_view("revision")].as_sint().value_or(-1),
        1);
    EXPECT_EQ(
        (*authoring)[std::string_view("lastChangeKind")].as_string().value_or(
            ""),
        std::string_view("setValue"));
    EXPECT_FALSE(authoring->contains(std::string_view("kind")));
    EXPECT_FALSE(authoring->contains(std::string_view("migration")));
    EXPECT_TRUE(authoring->contains(std::string_view("sourceFingerprint")));

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    ASSERT_TRUE(WaitForExecutions(1, std::chrono::seconds(2)));
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
    // Phase 84: Apply returns document inline, so Scheduler never falls back to
    // a separate GetDocument call.
    EXPECT_EQ(shared_state_->get_document_count, 0);
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_EQ(shared_state_->get_last_compile_purpose(), "execution");
    EXPECT_EQ(shared_state_->get_last_props_key1_value(), "compiled");
}

TEST_F(SchedulerRuntimeBackedTest, SchedulerAuthoringCompilePreviewOnly)
{
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] = FactoryTaskGuidString;
    (*task0.as_object())[std::string_view("pluginGuid")] =
        FactoryPluginGuidString;
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto                result = scheduler_->CompileTaskAuthoring(0, request);
    auto                obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    auto compile = (*obj)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile.has_value());
    EXPECT_TRUE((*compile)[std::string_view("ok")].as_bool().value_or(false));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_TRUE((shared_state_->executed_count == 0));
}
namespace
{
    yyjson::value MakeRepositoryCreateRequest(
        std::string_view display_name,
        int64_t          retry_count = 3)
    {
        yyjson::value request(Das::Utils::MakeYyjsonObject());
        (*request.as_object())[std::string_view("pluginGuid")] =
            FactoryPluginGuidString;
        (*request.as_object())[std::string_view("taskTypeGuid")] =
            FactoryTaskGuidString;
        (*request.as_object())[std::string_view("displayName")] =
            std::string(display_name);

        yyjson::value initial(Das::Utils::MakeYyjsonObject());
        (*initial.as_object())[std::string_view("retryCount")] = retry_count;
        (*request.as_object())[std::string_view("initialProperties")] =
            std::move(initial);
        return request;
    }

    yyjson::value MakeRepositoryRenameRequest(std::string_view display_name)
    {
        yyjson::value request(Das::Utils::MakeYyjsonObject());
        (*request.as_object())[std::string_view("displayName")] =
            std::string(display_name);
        return request;
    }

    yyjson::value MakeRepositoryAuthoringChange(
        std::optional<int64_t> base_revision,
        std::string_view       kind = "setValue")
    {
        yyjson::value change(Das::Utils::MakeYyjsonObject());
        if (base_revision)
        {
            (*change.as_object())[std::string_view("baseRevision")] =
                *base_revision;
        }
        yyjson::value element(Das::Utils::MakeYyjsonObject());
        (*element.as_object())[std::string_view("kind")] =
            std::make_pair(std::string_view(kind), yyjson::copy_string);
        yyjson::value payload(Das::Utils::MakeYyjsonObject());
        // Phase 84 strict change payloads: setValue requires target/fieldName/
        // value; invokeCommand requires commandId. The legacy empty-payload
        // shape is rejected by ValidateAuthoringChange before the Provider is
        // reached.
        if (kind == std::string_view("setValue"))
        {
            yyjson::value target(Das::Utils::MakeYyjsonObject());
            (*target.as_object())[std::string_view("kind")] = "document";
            (*payload.as_object())[std::string_view("target")] =
                std::move(target);
            (*payload.as_object())[std::string_view("fieldName")] = "key1";
            (*payload.as_object())[std::string_view("value")] = "edited";
        }
        else if (kind == std::string_view("invokeCommand"))
        {
            (*payload.as_object())[std::string_view("commandId")] = "test";
        }
        (*element.as_object())[std::string_view("payload")] =
            std::move(payload);
        auto changes = Das::Utils::MakeYyjsonArray();
        (*changes.as_array()).emplace_back(std::move(element));
        (*change.as_object())[std::string_view("changes")] = std::move(changes);
        return change;
    }

    void SeedRepositoryEntry(
        Das::Core::SettingsManager::SettingsManager& settings,
        int64_t                                      entry_id,
        std::string_view                             plugin_guid,
        std::string_view                             task_type_guid,
        std::string_view                             display_name)
    {
        yyjson::value entry(Das::Utils::MakeYyjsonObject());
        (*entry.as_object())[std::string_view("entryId")] = entry_id;
        (*entry.as_object())[std::string_view("displayName")] =
            std::string(display_name);
        (*entry.as_object())[std::string_view("pluginGuid")] =
            std::string(plugin_guid);
        (*entry.as_object())[std::string_view("taskTypeGuid")] =
            std::string(task_type_guid);
        {
            yyjson::value authoring(Das::Utils::MakeYyjsonObject());
            (*authoring.as_object())[std::string_view("revision")] = 5;
            // Phase 84 authoring metadata is revision + optional lastChangeKind
            // + legacy best-effort sourceFingerprint consumed by the
            // repository-reference reconciliation wire. The legacy
            // kind/migrationState fields are no longer part of the persisted
            // wire.
            (*authoring.as_object())[std::string_view("lastChangeKind")] =
                "setValue";
            (*authoring.as_object())[std::string_view("sourceFingerprint")] =
                "seed-source";
            (*entry.as_object())[std::string_view("authoring")] =
                std::move(authoring);
        }
        (*entry.as_object())[std::string_view("acceptedProperties")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        // RepositoryEntryDto 的 graph_document 是 required 字段，caster 反
        // 序列化时要求 key 存在；生产 CreateEntry 用默认（空）值序列化，
        // seed 同样补空对象，否则后续 ListEntries 反序列化抛
        // "JSON object key not found: graphDocument"。历史存量的
        // compiledArtifact 键按未知键静默忽略（字段已退役——编译产物永不
        // 落盘）。
        (*entry.as_object())[std::string_view("graphDocument")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        {
            yyjson::value availability(Das::Utils::MakeYyjsonObject());
            (*availability.as_object())[std::string_view("state")] =
                "available";
            (*entry.as_object())[std::string_view("availability")] =
                std::move(availability);
        }

        ASSERT_EQ(
            settings.UpdateTaskRepositoryEntryJson("0", entry_id, entry),
            DAS_S_OK);
    }

    yyjson::value SingleRepositoryEntry(const yyjson::value& repository)
    {
        auto repository_obj = repository.as_object();
        EXPECT_TRUE(repository_obj.has_value());
        auto entries =
            (*repository_obj)[std::string_view("entries")].as_array();
        EXPECT_TRUE(entries.has_value());
        EXPECT_EQ(entries->size(), 1u);
        return entries && !entries->empty()
                   ? (*entries)[0]
                   : yyjson::value(Das::Utils::MakeYyjsonObject());
    }

    void ExpectRepositoryAvailability(
        const yyjson::value& entry,
        std::string_view     reason)
    {
        auto entry_obj = entry.as_object();
        ASSERT_TRUE(entry_obj.has_value());
        auto availability =
            (*entry_obj)[std::string_view("availability")].as_object();
        ASSERT_TRUE(availability.has_value());
        EXPECT_EQ(
            (*availability)[std::string_view("state")].as_string().value_or(""),
            std::string_view("unavailable"));
        EXPECT_EQ(
            (*availability)[std::string_view("reason")].as_string().value_or(
                ""),
            reason);
        auto message = (*availability)[std::string_view("message")].as_string();
        ASSERT_TRUE(message.has_value());
        EXPECT_FALSE(message->empty());
    }

    void ExpectNoRepositoryAuthoringProviderCalls(FactoryTaskSharedState* state)
    {
        const ipc_scoped_lock lock(state->mutex);
        EXPECT_EQ(state->authoring_session_count, 0);
        EXPECT_EQ(state->get_document_count, 0);
        EXPECT_EQ(state->apply_change_count, 0);
        EXPECT_EQ(state->compile_count, 0);
        EXPECT_EQ(state->decoy_authoring_create_count, 0);
    }

    struct ProviderCallCounts
    {
        int authoring_session_count = 0;
        int get_document_count = 0;
        int apply_change_count = 0;
        int compile_count = 0;
        int decoy_authoring_create_count = 0;
    };

    ProviderCallCounts SnapshotProviderCallCounts(FactoryTaskSharedState* state)
    {
        const ipc_scoped_lock lock(state->mutex);
        return ProviderCallCounts{
            state->authoring_session_count,
            state->get_document_count,
            state->apply_change_count,
            state->compile_count,
            state->decoy_authoring_create_count};
    }

    void ExpectProviderCallCountsUnchanged(
        FactoryTaskSharedState*   state,
        const ProviderCallCounts& before)
    {
        const ipc_scoped_lock lock(state->mutex);
        EXPECT_EQ(
            state->authoring_session_count,
            before.authoring_session_count);
        EXPECT_EQ(state->get_document_count, before.get_document_count);
        EXPECT_EQ(state->apply_change_count, before.apply_change_count);
        EXPECT_EQ(state->compile_count, before.compile_count);
        EXPECT_EQ(
            state->decoy_authoring_create_count,
            before.decoy_authoring_create_count);
    }

    void ExpectRepositoryAuthoringRejectedWithoutProviderCalls(
        SchedulerService&       scheduler,
        FactoryTaskSharedState* state,
        std::string_view        error_kind)
    {
        auto before = SnapshotProviderCallCounts(state);

        const yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto                document =
            scheduler.GetRepositoryEntryAuthoringDocument(0, request);
        auto document_obj = document.as_object();
        ASSERT_TRUE(document_obj.has_value());
        EXPECT_EQ(
            (*document_obj)[std::string_view("errorKind")].as_string().value_or(
                ""),
            error_kind);

        auto change = MakeRepositoryAuthoringChange(5);
        auto apply = scheduler.ApplyRepositoryEntryAuthoringChange(0, change);
        auto apply_obj = apply.as_object();
        ASSERT_TRUE(apply_obj.has_value());
        EXPECT_EQ(
            (*apply_obj)[std::string_view("errorKind")].as_string().value_or(
                ""),
            error_kind);

        ExpectProviderCallCountsUnchanged(state, before);
    }

    void ExpectRepositoryCompileRejectedWithoutProviderCalls(
        SchedulerService&       scheduler,
        FactoryTaskSharedState* state,
        std::string_view        error_kind)
    {
        auto before = SnapshotProviderCallCounts(state);

        const yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto compile = scheduler.CompileRepositoryEntryAuthoring(0, request);
        auto compile_obj = compile.as_object();
        ASSERT_TRUE(compile_obj.has_value());
        EXPECT_EQ(
            (*compile_obj)[std::string_view("errorKind")].as_string().value_or(
                ""),
            error_kind);

        ExpectProviderCallCountsUnchanged(state, before);
    }

    void ExpectUnavailableRenameDeleteWithoutProviderCalls(
        SchedulerService&            scheduler,
        FactoryTaskSharedState*      state,
        const std::filesystem::path& settings_dir,
        std::string_view             reason)
    {
        auto entry = SingleRepositoryEntry(scheduler.GetTaskRepository());
        ExpectRepositoryAvailability(entry, reason);
        ExpectNoRepositoryAuthoringProviderCalls(state);

        auto renamed = scheduler.RenameRepositoryEntry(
            0,
            MakeRepositoryRenameRequest("Renamed unavailable entry"));
        auto renamed_obj = renamed.as_object();
        ASSERT_TRUE(renamed_obj.has_value());
        EXPECT_EQ(
            (*renamed_obj)[std::string_view("displayName")]
                .as_string()
                .value_or(""),
            std::string_view("Renamed unavailable entry"));
        ExpectRepositoryAvailability(renamed, reason);
        ExpectNoRepositoryAuthoringProviderCalls(state);

        EXPECT_EQ(scheduler.DeleteRepositoryEntry(0), DAS_S_OK);
        EXPECT_FALSE(
            std::filesystem::exists(
                settings_dir / DAS_PATH_LITERAL("0")
                / DAS_PATH_LITERAL("taskRepository0.json")));
        auto repository = scheduler.GetTaskRepository();
        auto entries =
            (*repository.as_object())[std::string_view("entries")].as_array();
        ASSERT_TRUE(entries.has_value());
        EXPECT_TRUE(entries->empty());
        ExpectNoRepositoryAuthoringProviderCalls(state);
    }
} // namespace
// ===========================================================================
// Phase 84 Wave 03 — strict Authoring wire (D-84-04/05/06/08/09/10/11/32)
// ---------------------------------------------------------------------------
// These fixtures flip the test plugin into strict Provider-result mode and
// assert that SchedulerService preserves the Provider envelope, validates
// Apply output before any persistence, and never merges a Core-authored
// catalog. The malicious-output variants prove that invalid Apply results
// leave persisted task/repository state byte-for-byte untouched.
// ===========================================================================

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringGetPreservesStrictProviderResultEnvelope)
{
    SeedEmptyAuthoringTask(*settings_manager_, 0, 0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto result = scheduler_->GetTaskAuthoringDocument(0, request);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ((*obj)[std::string_view("taskId")].as_sint().value_or(-1), 0);
    EXPECT_TRUE(obj->contains(std::string_view("document")));
    EXPECT_TRUE(obj->contains(std::string_view("diagnostics")));
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(
        (*document)[std::string_view("viewKind")].as_string().value_or(""),
        std::string_view("formSequence"));
    EXPECT_EQ(
        (*document)[std::string_view("contractVersion")].as_sint().value_or(-1),
        1);
    EXPECT_EQ(
        (*document)[std::string_view("revision")].as_sint().value_or(-1),
        0);
    // Scheduler must not synthesise a kind/catalog/migration on top of the
    // Provider envelope.
    EXPECT_FALSE(document->contains(std::string_view("kind")));
    EXPECT_FALSE(document->contains(std::string_view("catalog")));
    EXPECT_FALSE(document->contains(std::string_view("migration")));
    EXPECT_FALSE(document->contains(std::string_view("sourceFingerprint")));
    auto diagnostics = (*obj)[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(diagnostics.has_value());

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->authoring_session_count, 1);
    EXPECT_EQ(shared_state_->get_document_count, 1);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringApplyValidatesResultBeforePersisting)
{
    SeedEmptyAuthoringTask(*settings_manager_, 0, 0, "initial");
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->apply_omit_document = true;
    }

    auto change = MakeStrictAuthoringChange(0);
    auto result = scheduler_->ApplyTaskAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_FALSE((*obj)[std::string_view("ok")].as_bool().value_or(true));
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("missingField"));

    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    auto props = (*persisted_obj)[std::string_view("properties")].as_object();
    ASSERT_TRUE(props.has_value());
    EXPECT_EQ(
        (*props)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("initial"));
    auto authoring =
        (*persisted_obj)[std::string_view("authoring")].as_object();
    if (authoring.has_value())
    {
        EXPECT_EQ(
            (*authoring)[std::string_view("revision")].as_sint().value_or(-1),
            0);
    }

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringApplyRejectsWrongDocumentRevisionWithoutPersisting)
{
    SeedEmptyAuthoringTask(*settings_manager_, 0, 0, "initial");
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->apply_document_revision_delta = 1;
    }

    auto change = MakeStrictAuthoringChange(0);
    auto result = scheduler_->ApplyTaskAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_FALSE((*obj)[std::string_view("ok")].as_bool().value_or(true));
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("revisionMismatch"));

    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    auto props = (*persisted_obj)[std::string_view("properties")].as_object();
    ASSERT_TRUE(props.has_value());
    EXPECT_EQ(
        (*props)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("initial"));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringApplyPersistsStrictProviderResult)
{
    SeedEmptyAuthoringTask(*settings_manager_, 0, 0, "initial");
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);

    auto change = MakeStrictAuthoringChange(0);
    auto result = scheduler_->ApplyTaskAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
    EXPECT_TRUE(obj->contains(std::string_view("acceptedProperties")));
    EXPECT_TRUE(obj->contains(std::string_view("document")));
    EXPECT_TRUE(obj->contains(std::string_view("diagnostics")));
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(
        (*document)[std::string_view("viewKind")].as_string().value_or(""),
        std::string_view("formSequence"));
    EXPECT_EQ(
        (*document)[std::string_view("revision")].as_sint().value_or(-1),
        1);
    auto accepted = (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(
        (*accepted)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("accepted"));

    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    auto persisted_props =
        (*persisted_obj)[std::string_view("properties")].as_object();
    ASSERT_TRUE(persisted_props.has_value());
    EXPECT_EQ(
        (*persisted_props)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("accepted"));
    auto persisted_authoring =
        (*persisted_obj)[std::string_view("authoring")].as_object();
    ASSERT_TRUE(persisted_authoring.has_value());
    EXPECT_EQ(
        (*persisted_authoring)[std::string_view("revision")].as_sint().value_or(
            -1),
        1);
    EXPECT_EQ(
        (*persisted_authoring)[std::string_view("lastChangeKind")]
            .as_string()
            .value_or(""),
        std::string_view("setValue"));
    EXPECT_FALSE(persisted_authoring->contains(std::string_view("kind")));
    EXPECT_FALSE(persisted_authoring->contains(std::string_view("migration")));
    EXPECT_TRUE(
        persisted_authoring->contains(std::string_view("sourceFingerprint")));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringCompilePreservesStrictProviderResult)
{
    SeedEmptyAuthoringTask(*settings_manager_, 0, 0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto                result = scheduler_->CompileTaskAuthoring(0, request);
    auto                obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_TRUE(obj->contains(std::string_view("compile")));
    EXPECT_TRUE(obj->contains(std::string_view("diagnostics")));
    auto compile = (*obj)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile.has_value());
    EXPECT_TRUE((*compile)[std::string_view("ok")].as_bool().value_or(false));
    EXPECT_TRUE(compile->contains(std::string_view("executionInput")));
    EXPECT_FALSE(obj->contains(std::string_view("providerDebug")));
    auto diagnostics = (*obj)[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(diagnostics.has_value());
    ASSERT_EQ(diagnostics->size(), 1u);

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->compile_count, 1);
}

// ===========================================================================
// Backend end-to-end Get → Apply → Get regression.
//
// Prove that the strict Phase 84 wire holds across the full
// Provider → Scheduler → persisted store → new Provider session → Get
// cycle. The neighboring 84-03 tests cover each step in isolation; this
// test chains Get → Apply → Get to assert the new revision is observable
// on a follow-up Get. The fake SchedulerTestPlugin speaks the strict wire
// so the assertion exercises the real Scheduler atomic apply +
// validate-before-persist ordering without depending on the MaaPI plugin.
// The MaaPI fixture pair (interface_authoring_v28.jsonc +
// post84_accepted_properties.json) covers the same cycle on the real
// Provider in
// MaapiAuthoringTest.Post84DisposableStateRoundTripsProviderGetApplyGet.
//
// Note: this test requires the IPC subprocess (DasHost.exe) to load
// FactoryPlugin.dll. ctest auto-injects DAS_HOST_EXE_PATH /
// DAS_PLUGIN_DIR; direct exe invocation without those env vars produces
// DAS_E_IPC_PLUGIN_LOAD_FAILED (-1080000101) at Initialize.
// ===========================================================================
TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringGetApplyGetCyclePreservesStrictEnvelope)
{
    SeedEmptyAuthoringTask(*settings_manager_, 0, 0, "initial");
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);

    // Get #1: initial revision is 0.
    {
        const yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")));
        auto document = (*obj)[std::string_view("document")].as_object();
        ASSERT_TRUE(document.has_value());
        EXPECT_EQ(
            (*document)[std::string_view("revision")].as_sint().value_or(-1),
            0);
        EXPECT_EQ(
            (*document)[std::string_view("viewKind")].as_string().value_or(""),
            std::string_view("formSequence"));
    }

    // Apply: bumps revision 0 → 1 and persists acceptedProperties.
    {
        auto change = MakeStrictAuthoringChange(0);
        auto result = scheduler_->ApplyTaskAuthoringChange(0, change);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
        auto document = (*obj)[std::string_view("document")].as_object();
        ASSERT_TRUE(document.has_value());
        EXPECT_EQ(
            (*document)[std::string_view("revision")].as_sint().value_or(-1),
            1);
    }

    // Get #2: follow-up Get observes the new persisted revision. Proves the
    // Scheduler wrote the metadata before the second Provider session was
    // created, and that the new session received that revision via the
    // context (SchedulerService::MakeAuthoringContextJson).
    {
        const yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")));
        auto document = (*obj)[std::string_view("document")].as_object();
        ASSERT_TRUE(document.has_value());
        EXPECT_EQ(
            (*document)[std::string_view("revision")].as_sint().value_or(-1),
            1);
        EXPECT_EQ(
            (*document)[std::string_view("viewKind")].as_string().value_or(""),
            std::string_view("formSequence"));
    }

    // Persisted metadata reflects revision 1 + lastChangeKind=setValue.
    {
        auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
        auto persisted_obj = persisted.as_object();
        ASSERT_TRUE(persisted_obj.has_value());
        auto persisted_authoring =
            (*persisted_obj)[std::string_view("authoring")].as_object();
        ASSERT_TRUE(persisted_authoring.has_value());
        EXPECT_EQ(
            (*persisted_authoring)[std::string_view("revision")]
                .as_sint()
                .value_or(-1),
            1);
        EXPECT_EQ(
            (*persisted_authoring)[std::string_view("lastChangeKind")]
                .as_string()
                .value_or(""),
            std::string_view("setValue"));
    }

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->get_document_count, 2);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryAuthoringGetPreservesStrictProviderResultEnvelope)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);

    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Strict repository entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto result = scheduler_->GetRepositoryEntryAuthoringDocument(0, request);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ((*obj)[std::string_view("entryId")].as_sint().value_or(-1), 0);
    EXPECT_TRUE(obj->contains(std::string_view("document")));
    EXPECT_TRUE(obj->contains(std::string_view("diagnostics")));
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(
        (*document)[std::string_view("viewKind")].as_string().value_or(""),
        std::string_view("formSequence"));
    EXPECT_FALSE(document->contains(std::string_view("kind")));
    EXPECT_FALSE(document->contains(std::string_view("catalog")));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->get_document_count, 1);
}

// creator 后端 resolver 的条目分支必须走干净会话创建（纯函数会话
// canon）——编译链（注入 pass/投影 fetcher）拿到的会话是调
// 用方私有、用完即弃的新建实例，禁止消费 HTTP 编辑工作流的共享缓存
// repository_authoring_sessions_。预置一个缓存命中（先经仓库条目 Get 把
// 会话灌进缓存），再经 creator 通道开同条目会话：干净创建必须新造工厂
// 会话（authoring_session_count 1→2，context 从 store 权威读数现算），
// 而非把缓存实例 AddRef 交出（计数停留 1）。
TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryAuthoringCreatorSessionIsCleanNotCached)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);

    // 条目 id 从 0 起分配，而 resolver 条目分支的入口判据是
    // repositoryEntryId > 0——建两个条目、用 id=1 走真条目分支。
    auto first_created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Creator clean session filler", 8));
    ASSERT_EQ(
        (*first_created.as_object())[std::string_view("entryId")]
            .as_sint()
            .value_or(-1),
        0);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Creator clean session entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        1);

    // 缓存预置：一次仓库条目 Get 经 HTTP 管线在 repository_authoring_
    // sessions_ 建立共享会话（revision/locale 戳一致时后续复用）。
    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto primed = scheduler_->GetRepositoryEntryAuthoringDocument(1, request);
    ASSERT_TRUE(primed.as_object().has_value());
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        ASSERT_EQ(shared_state_->authoring_session_count, 1);
    }

    // creator 通道入参（编译闭包 pass 同款形状）：条目绑定键由调用方携
    // 带，repo 装配键（properties/revision/actionCatalog 等）由后端从
    // store 现读。
    yyjson::value context(Das::Utils::MakeYyjsonObject());
    (*context.as_object())[std::string_view("repositoryEntryId")] =
        static_cast<std::int64_t>(1);
    auto context_json = Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
        Das::Utils::CloneYyjsonValue(context));

    Das::DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;

    // 经 creator 后端消费门开条目会话（resolver 委托终点，与插件侧
    // creator->CreateAuthoringSession 同一入口）。
    const auto session_result =
        plugin_manager_->GetTaskComponentFactoryManager()
            .CreateAuthoringSession(
                FactoryTaskGuid,
                context_json.Get(),
                session.Put());
    ASSERT_EQ(session_result, DAS_S_OK);
    ASSERT_NE(session.Get(), nullptr);

    // 干净会话语义：新造工厂会话（1→2），context 装配源自 store 权威读
    // 数（revision=0——建条目后未 Apply）。
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->authoring_session_count, 2);
        EXPECT_EQ(shared_state_->last_context_revision, 0);
    }

    // 新实例可用：GetDocument 走通（编译链早段同款消费面）。
    Das::DasPtr<Das::ExportInterface::IDasJson> document_json;
    ASSERT_EQ(session->GetDocument(nullptr, document_json.Put()), DAS_S_OK);
}

// ---------------------------------------------------------------------
// 注入链回归：图任务引用 MaaPi form 条目的编译契约（实测缺陷锁定——
// 修复前整图编译被「referenced entry 17 document carries no root ports
// section」（ReferenceDocumentMissing）拒绝，execution_order 清空）。
//
// 边界场景形态（内联 fixture，与实测缺陷现场的文档逐字段同构）：父图
// 五节点链 begin→launch→toast→maapi.run（n_2 绑 repositoryRef=17）+ 一条
// 数据边；被引条目为 MaaPi form 条目（taskTypeGuid 69F20001、
// graphDocument null、acceptedProperties 纯 form 值域——非图文档，faces
// 预备扫描对其天然空表 fail-open）。两条用例各锁一半行为契约：
//  - 绿色用例：条目补 adapter.interfacePath（唯一一处字段差异——无
//    interfacePath 时 TryParseCatalog 恒 nullopt，MaaPi 编译 ok:false，
//    注入链产物半段按 fail-closed 设计拒绝，编译产物物理不存在）→
//    整图编译成功、executionOrder 非空、产物广播绑定携带 envelope。
//  - 原样用例：条目保持无项目形态 → 编译按设计 fail-closed，但诊断必须
//    命中真根因（编译产物缺失），而不是「无根 ports 节」（文档自述契约
//    已由组件语境投影满足——零暴露输入的 form 条目也自述空口面）。
// ---------------------------------------------------------------------

// MaaPi form 条目最小形态：pi 零任务、无 adapter.interfacePath（「未配置
// 项目」边界——编译产物不可构造），availability available。
constexpr std::string_view kRealDataEntry17Json = R"json({
  "entryId": 17,
  "displayName": "Maa 执行",
  "pluginGuid": "69F20000-0000-4000-8000-000000000001",
  "taskTypeGuid": "69F20001-0000-4000-8000-000000000001",
  "authoring": {
    "revision": 2,
    "lastChangeKind": "uiEvent",
    "sourceFingerprint": ""
  },
  "acceptedProperties": {
    "version": 1,
    "adapter": {
      "executionPolicy": {
        "failFast": true
      }
    },
    "pi": {
      "globalOptions": [],
      "resourceOptions": [],
      "controllerOptions": [],
      "tasks": [],
      "orphanPaths": []
    },
    "ui": {
      "values": {
        "sectionExpand.project": true
      }
    }
  },
  "graphDocument": null,
  "compiledArtifact": null,
  "availability": {
    "state": "available",
    "reason": "",
    "message": ""
  }
})json";

// 父图任务（das.graph 类型）最小形态：单入口 begin 的五节点信号链 +
// 一条数据边，n_2 = maapi.run 组件节点（settings.repositoryRef=17——闭包
// pass 的绑定判据）。
constexpr std::string_view kRealDataTask63Json = R"json({
  "id": 63,
  "taskGuid": "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3",
  "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D",
  "nextExecutionTime": -1,
  "enabled": true,
  "properties": {
    "documentId": "",
    "version": 23,
    "fingerprint": "",
    "nodes": [
      {
        "nodeId": "n_0",
        "target": {
          "targetKind": "componentRef",
          "componentRef": {
            "kind": "componentRef",
            "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
          }
        },
        "settings": null,
        "label": null
      },
      {
        "nodeId": "n_1",
        "target": {
          "targetKind": "componentRef",
          "componentRef": {
            "kind": "componentRef",
            "componentGuid": "95FD2FFA-88F2-4F85-A536-C708D25B641F",
            "pluginGuid": ""
          }
        },
        "settings": {
          "exePath": "E:\\games\\HYPERGRYPH\\Endfield Game\\Endfield.exe",
          "captureOutput": true
        },
        "label": null
      },
      {
        "nodeId": "n_2",
        "target": {
          "targetKind": "componentRef",
          "componentRef": {
            "kind": "componentRef",
            "componentGuid": "69F20008-0000-4000-8000-000000000001",
            "pluginGuid": ""
          }
        },
        "settings": {
          "repositoryRef": 17
        },
        "label": null
      },
      {
        "nodeId": "n_3",
        "target": {
          "targetKind": "componentRef",
          "componentRef": {
            "kind": "componentRef",
            "componentGuid": "CFAF95AF-5ABC-422C-8CDB-0346F0526236",
            "pluginGuid": ""
          }
        },
        "settings": {
          "error": false,
          "warning": false
        },
        "label": null
      },
      {
        "nodeId": "n_4",
        "target": {
          "targetKind": "componentRef",
          "componentRef": {
            "kind": "componentRef",
            "componentGuid": "68F1000C-0000-4000-8000-00000000000C",
            "pluginGuid": ""
          }
        },
        "settings": {
          "value": "测试"
        },
        "label": null
      }
    ],
    "edges": [
      {
        "edgeId": "edge::n_0::signal_out::n_1::signal_in",
        "sourceNodeId": "n_0",
        "sourcePortId": "signal_out",
        "targetNodeId": "n_1",
        "targetPortId": "signal_in",
        "edgeType": "signal"
      },
      {
        "edgeId": "edge::n_1::signal_out::n_3::signal_in",
        "sourceNodeId": "n_1",
        "sourcePortId": "signal_out",
        "targetNodeId": "n_3",
        "targetPortId": "signal_in",
        "edgeType": "signal"
      },
      {
        "edgeId": "edge::n_3::signal_out::n_2::signal_in",
        "sourceNodeId": "n_3",
        "sourcePortId": "signal_out",
        "targetNodeId": "n_2",
        "targetPortId": "signal_in",
        "edgeType": "signal"
      },
      {
        "edgeId": "edge::n_4::value::n_3::message",
        "sourceNodeId": "n_4",
        "sourcePortId": "value",
        "targetNodeId": "n_3",
        "targetPortId": "message",
        "edgeType": "data"
      }
    ],
    "tags": [],
    "viewData": {
      "positions": {
        "n_0": {
          "x": 39.0,
          "y": 166.0
        },
        "n_1": {
          "x": 223.0,
          "y": 184.0
        },
        "n_2": {
          "x": 647.0,
          "y": 112.0
        },
        "n_3": {
          "x": 405.0,
          "y": 142.0
        },
        "n_4": {
          "x": 192.0,
          "y": 303.0
        }
      }
    }
  },
  "authoring": {
    "revision": 23,
    "lastChangeKind": "setValue",
    "sourceFingerprint": ""
  }
})json";

static std::optional<yyjson::value> ParseRealDataFixture(
    std::string_view fixture_json)
{
    return Das::Utils::ParseYyjsonFromString(fixture_json);
}

// 从编译产物外壳（{"fingerprint","plan"}）取 plan 子树；解析失败返回
// nullopt（executionInput 形态：对象直取，字符串先 parse——两条形态
// ParseCompileExecutionInput 都接受）。
static std::optional<yyjson::value> UnwrapCompiledPlanShell(
    const yyjson::value& execution_input)
{
    yyjson::value shell = execution_input;
    if (const auto text = execution_input.as_string())
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(*text);
        if (!parsed.has_value())
        {
            return std::nullopt;
        }
        shell = std::move(*parsed);
    }
    auto shell_obj = shell.as_object();
    if (!shell_obj || !shell_obj->contains(std::string_view("plan")))
    {
        return std::nullopt;
    }
    return Das::Utils::CloneYyjsonValue((*shell_obj)[std::string_view("plan")]);
}

// 绿色场景：可编译形态（条目 17 补 fixture 项目路径，见上注释）→ 编译
// 成功、executionOrder 非空、绑条目记档在场（n_2 → 17——执行时编译段的
// 驱动数据）且产物零寄存（无 executionInput 目标绑定）。
TEST_F(
    SchedulerRuntimeBackedTest,
    RealDataTask63WithConfiguredEntry17RecordsBoundEntry)
{
    // 部署真实三插件（DasGraphTask：das.graph 类型+begin/end+focus log；
    // DasFlowControl：launch/value 组件；DasMaaPi：maapi.task 类型+run 组
    // 件——整目录部署保 Maa* 运行库与插件同址）。
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);
    DeployRealPluginArtifacts(
        "libDasFlowControl.dll",
        "DasFlowControl.json",
        /*force_ipc=*/false,
        "DasFlowControl");
    {
        namespace fs = std::filesystem;
        const fs::path src =
            IpcTestConfig::GetPluginDir() / DAS_PATH_LITERAL("DasMaaPi");
        ASSERT_TRUE(fs::exists(src))
            << "real DasMaaPi plugin dir not found at: "
            << Das::Utils::PathToUtf8(src);
        fs::copy(
            src,
            plugin_dir_ / DAS_PATH_LITERAL("DasMaaPi"),
            fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }

    auto entry17 = ParseRealDataFixture(kRealDataEntry17Json);
    ASSERT_TRUE(entry17.has_value()) << "entry 17 fixture unparsable";
    {
        // 补 fixture PI 项目（形状照 DasMaaPi 既有 fixture
        // interface_authoring.jsonc：controller/resource/默认勾选 task），
        // 使 MaaPi 编译可产出 envelope（相对 kRealDataEntry17Json 的唯一
        // 字段差异，见文件头注释）。
        namespace fs = std::filesystem;
        const fs::path project_dir = test_dir_ / DAS_PATH_LITERAL("pi_project");
        std::error_code ec;
        fs::create_directories(project_dir, ec);
        ASSERT_FALSE(ec) << ec.message();
        {
            std::ofstream ofs(
                project_dir / DAS_PATH_LITERAL("interface.json"),
                std::ios::trunc);
            ofs << R"json({
                "interface_version": 2,
                "name": "RealDataFixture",
                "version": "1.0",
                "controller": [{"name": "AdbCtl", "type": "Adb"}],
                "resource": [{"name": "Official", "path": "resource"}],
                "task": [{"name": "DefaultTask", "entry": "Run",
                          "default_check": true}]
            })json";
        }
        auto entry_obj = entry17->as_object();
        ASSERT_TRUE(entry_obj.has_value());
        auto accepted = Das::Utils::CloneYyjsonValue(
            (*entry_obj)[std::string_view("acceptedProperties")]);
        auto accepted_obj = accepted.as_object();
        ASSERT_TRUE(accepted_obj.has_value());
        auto adapter = Das::Utils::CloneYyjsonValue(
            (*accepted_obj)[std::string_view("adapter")]);
        auto adapter_obj = adapter.as_object();
        ASSERT_TRUE(adapter_obj.has_value());
        (*adapter_obj)[std::string_view("interfacePath")] = yyjson::value(
            Das::Utils::PathToUtf8(
                project_dir / DAS_PATH_LITERAL("interface.json")));
        (*accepted_obj)[std::string_view("adapter")] = std::move(adapter);
        (*entry_obj)[std::string_view("acceptedProperties")] =
            std::move(accepted);
    }
    ASSERT_EQ(
        settings_manager_->UpdateTaskRepositoryEntryJson("0", 17, *entry17),
        DAS_S_OK);

    auto task63 = ParseRealDataFixture(kRealDataTask63Json);
    ASSERT_TRUE(task63.has_value()) << "task 63 fixture unparsable";
    WriteSchedulerState(*settings_manager_, 64, {63}, {*task63});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    const auto          result = scheduler_->CompileTaskAuthoring(63, request);
    auto                obj = result.as_object();
    ASSERT_TRUE(obj.has_value())
        << "task 63 compile failed: "
        << Das::Utils::SerializeYyjsonValue(result).value_or("");
    ASSERT_TRUE(obj->contains(std::string_view("compile")))
        << Das::Utils::SerializeYyjsonValue(result).value_or("");
    auto compile_obj = (*obj)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile_obj.has_value());
    ASSERT_TRUE(compile_obj->contains(std::string_view("executionInput")))
        << "task 63 compile carries no executionInput: "
        << Das::Utils::SerializeYyjsonValue(result).value_or("");

    const auto plan = UnwrapCompiledPlanShell(
        (*compile_obj)[std::string_view("executionInput")]);
    ASSERT_TRUE(plan.has_value());
    auto plan_obj = plan->as_object();
    ASSERT_TRUE(plan_obj.has_value());
    ASSERT_TRUE(plan_obj->contains(std::string_view("executionOrder")))
        << "compiled plan carries no executionOrder";
    auto order = (*plan_obj)[std::string_view("executionOrder")].as_array();
    ASSERT_TRUE(order.has_value());
    EXPECT_FALSE(order->empty())
        << "execution_order cleared — compile was rejected";

    // 绑条目记档：referenceEntries 携 n_2 → 17（执行时编译段照单驱动）；
    // 产物零寄存——executionInput 目标绑定不得携带非空 default（声明口的
    // 派生悬空广播 null 默认合法在场）。
    {
        auto reference_entries =
            (*plan_obj)[std::string_view("referenceEntries")].as_array();
        ASSERT_TRUE(reference_entries.has_value())
            << "compiled plan carries no referenceEntries";
        ASSERT_EQ(reference_entries->size(), 1u);
        auto entry = (*reference_entries)[0].as_object();
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(
            (*entry)[std::string_view("nodeId")].as_string().value_or(""),
            "n_2");
        EXPECT_EQ(
            (*entry)[std::string_view("entryId")].as_int().value_or(0),
            17);
    }
    {
        auto bindings =
            (*plan_obj)[std::string_view("bindingPlan")].as_object();
        ASSERT_TRUE(bindings.has_value());
        auto arr = (*bindings)[std::string_view("bindings")].as_array();
        ASSERT_TRUE(arr.has_value());
        for (const auto& binding : *arr)
        {
            auto b = binding.as_object();
            ASSERT_TRUE(b.has_value());
            if ((*b)[std::string_view("targetPortId")].as_string()
                != std::string_view("executionInput"))
            {
                continue;
            }
            EXPECT_TRUE((*b)[std::string_view("defaultValue")].is_null())
                << "executionInput binding must not carry an artifact "
                   "(zero artifact registration)";
        }
    }
}

// 原样场景（条目 17 fixture 零字段覆盖——保持未配置 PI 项目形态）：
// 可编译性到节点 Do 才报（编译不再取被引条目产物——口面半段只
// GetDocument，文档自述契约由组件语境投影满足）→ 编译照常成功、记档在
// 场；条目未配置的失败面移至执行时编译（节点失败路径，116-06 深验）。
TEST_F(
    SchedulerRuntimeBackedTest,
    RealDataTask63VerbatimEntry17CompilesGreenUntilExecution)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);
    DeployRealPluginArtifacts(
        "libDasFlowControl.dll",
        "DasFlowControl.json",
        /*force_ipc=*/false,
        "DasFlowControl");
    {
        namespace fs = std::filesystem;
        const fs::path src =
            IpcTestConfig::GetPluginDir() / DAS_PATH_LITERAL("DasMaaPi");
        ASSERT_TRUE(fs::exists(src))
            << "real DasMaaPi plugin dir not found at: "
            << Das::Utils::PathToUtf8(src);
        fs::copy(
            src,
            plugin_dir_ / DAS_PATH_LITERAL("DasMaaPi"),
            fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }

    auto entry17 = ParseRealDataFixture(kRealDataEntry17Json);
    ASSERT_TRUE(entry17.has_value()) << "entry 17 fixture unparsable";
    ASSERT_EQ(
        settings_manager_->UpdateTaskRepositoryEntryJson("0", 17, *entry17),
        DAS_S_OK);
    auto task63 = ParseRealDataFixture(kRealDataTask63Json);
    ASSERT_TRUE(task63.has_value()) << "task 63 fixture unparsable";
    WriteSchedulerState(*settings_manager_, 64, {63}, {*task63});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    const auto          result = scheduler_->CompileTaskAuthoring(63, request);
    auto                obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE(obj->contains(std::string_view("compile")))
        << "task 63 compile failed before the reference closure pass: "
        << Das::Utils::SerializeYyjsonValue(result).value_or("");

    const auto diagnostics_text = Das::Utils::SerializeYyjsonValue(
                                      (*obj)[std::string_view("diagnostics")])
                                      .value_or("");
    EXPECT_EQ(
        diagnostics_text.find("carries no root ports section"),
        std::string::npos)
        << "the ports self-description contract must already hold for the "
           "referenced form entry: "
        << diagnostics_text;

    // 编译照常成功（零干跑预检——被引条目可编译性到节点 Do 才报）：
    // executionOrder 非空 + 绑条目记档在场（执行时编译段的驱动数据）。
    auto compile_obj = (*obj)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile_obj.has_value());
    ASSERT_TRUE(compile_obj->contains(std::string_view("executionInput")))
        << "task 63 compile carries no executionInput";
    const auto plan = UnwrapCompiledPlanShell(
        (*compile_obj)[std::string_view("executionInput")]);
    ASSERT_TRUE(plan.has_value());
    auto plan_obj = plan->as_object();
    ASSERT_TRUE(plan_obj.has_value());
    {
        auto order = (*plan_obj)[std::string_view("executionOrder")].as_array();
        ASSERT_TRUE(order.has_value());
        EXPECT_FALSE(order->empty())
            << "compile must stay green for an unconfigured referenced entry "
               "(failure moves to execution time)";
    }
    {
        auto reference_entries =
            (*plan_obj)[std::string_view("referenceEntries")].as_array();
        ASSERT_TRUE(reference_entries.has_value());
        ASSERT_EQ(reference_entries->size(), 1u);
        auto entry = (*reference_entries)[0].as_object();
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(
            (*entry)[std::string_view("nodeId")].as_string().value_or(""),
            "n_2");
        EXPECT_EQ(
            (*entry)[std::string_view("entryId")].as_int().value_or(0),
            17);
    }
}

// 图条目（GraphTask 类型，begin/end 带边界动态口）经 creator 通道 resolver
// 干净开会话（编译闭包 pass 同款形状）→ GetDocument 根 ports section 自述
// 边界口面：begin faces→direction=input、end faces→direction=output（投影
// 与 DeriveBoundaryPorts 单一事实源）。context 自足性钉：干净会话不经编辑
// 缓存，nodeFaces 全部经 context 键注入（条目图文档扫描——acceptedProperties
// 即图文档）。
TEST_F(
    SchedulerRuntimeBackedTest,
    GraphEntryCreatorResolverSessionSelfDescribesBoundaryPorts)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);
    DeployRealPluginArtifacts(
        "libDasFlowControl.dll",
        "DasFlowControl.json",
        /*force_ipc=*/false,
        "DasFlowControl");

    // 图条目（entry 7）：begin p1（string）/end q1（int）边界动态口——
    // acceptedProperties 即图文档（图条目存储形状）。
    {
        yyjson::value entry(Das::Utils::MakeYyjsonObject());
        (*entry.as_object())[std::string_view("entryId")] =
            static_cast<int64_t>(7);
        (*entry.as_object())[std::string_view("displayName")] =
            "Resolver graph entry";
        (*entry.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*entry.as_object())[std::string_view("taskTypeGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        {
            yyjson::value authoring(Das::Utils::MakeYyjsonObject());
            (*authoring.as_object())[std::string_view("revision")] = 5;
            (*authoring.as_object())[std::string_view("lastChangeKind")] =
                std::make_pair(
                    std::string_view("setValue"),
                    yyjson::copy_string);
            (*authoring.as_object())[std::string_view("sourceFingerprint")] =
                std::make_pair(
                    std::string_view("resolver-seed"),
                    yyjson::copy_string);
            (*entry.as_object())[std::string_view("authoring")] =
                std::move(authoring);
        }
        auto inner_properties = Das::Utils::ParseYyjsonFromString(R"json({
            "documentId": "resolver-inner",
            "version": 1,
            "nodes": [
                {
                    "nodeId": "inner_begin",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "boundaryPorts": [
                            {"id": "p1", "type": "string"}
                        ]
                    }
                },
                {
                    "nodeId": "inner_end",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "boundaryPorts": [
                            {"id": "q1", "type": "int"}
                        ]
                    }
                }
            ],
            "edges": [],
            "tags": []
        })json");
        ASSERT_TRUE(inner_properties.has_value());
        (*entry.as_object())[std::string_view("acceptedProperties")] =
            std::move(*inner_properties);
        (*entry.as_object())[std::string_view("graphDocument")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        {
            yyjson::value availability(Das::Utils::MakeYyjsonObject());
            (*availability.as_object())[std::string_view("state")] =
                "available";
            (*entry.as_object())[std::string_view("availability")] =
                std::move(availability);
        }
        ASSERT_EQ(
            settings_manager_->UpdateTaskRepositoryEntryJson("0", 7, entry),
            DAS_S_OK);
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // creator 通道入参（编译闭包 pass 同款形状）：条目绑定键在场，repo 装
    // 配键（properties/revision/actionCatalog/nodeFaces）由后端从 store 现
    // 读现算。
    yyjson::value context(Das::Utils::MakeYyjsonObject());
    (*context.as_object())[std::string_view("repositoryEntryId")] =
        static_cast<std::int64_t>(7);
    auto context_json = Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
        Das::Utils::CloneYyjsonValue(context));

    Das::DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;
    const auto                                                  session_result =
        plugin_manager_->GetTaskComponentFactoryManager()
            .CreateAuthoringSession(
                Das::Core::ForeignInterfaceHost::MakeDasGuid(
                    "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3"),
                context_json.Get(),
                session.Put());
    ASSERT_EQ(session_result, DAS_S_OK);
    ASSERT_NE(session.Get(), nullptr);

    Das::DasPtr<Das::ExportInterface::IDasJson> document_json;
    ASSERT_EQ(session->GetDocument(nullptr, document_json.Put()), DAS_S_OK);
    // IDasJson → 文本 → yyjson（ToString 全文取回）。
    yyjson::value document(Das::Utils::MakeYyjsonObject());
    {
        Das::DasPtr<IDasReadOnlyString> document_text;
        ASSERT_EQ(document_json->ToString(0, document_text.Put()), DAS_S_OK);
        ASSERT_TRUE(document_text);
        const char* utf8 = nullptr;
        ASSERT_EQ(document_text->GetUtf8(&utf8), DAS_S_OK);
        ASSERT_NE(utf8, nullptr);
        auto parsed = Das::Utils::ParseYyjsonFromString(utf8);
        ASSERT_TRUE(parsed.has_value());
        document = std::move(*parsed);
    }
    auto document_obj = document.as_object();
    ASSERT_TRUE(document_obj.has_value());
    ASSERT_TRUE(document_obj->contains(std::string_view("document")))
        << "graph entry GetDocument carries no document envelope";
    auto inner = (*document_obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(inner.has_value());
    ASSERT_TRUE(inner->contains(std::string_view("ports")))
        << "graph entry document carries no root ports section";
    auto ports = (*inner)[std::string_view("ports")].as_array();
    ASSERT_TRUE(ports.has_value());
    ASSERT_EQ(ports->size(), 2u);
    // begin faces→input、end faces→output（边界口对侧别由推导层裁定）。
    auto first = (*ports)[0].as_object();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ((*first)[std::string_view("id")].as_string().value_or(""), "p1");
    EXPECT_EQ(
        (*first)[std::string_view("direction")].as_string().value_or(""),
        "input");
    EXPECT_EQ(
        (*first)[std::string_view("type")].as_string().value_or(""),
        "string");
    auto second = (*ports)[1].as_object();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ((*second)[std::string_view("id")].as_string().value_or(""), "q1");
    EXPECT_EQ(
        (*second)[std::string_view("direction")].as_string().value_or(""),
        "output");
    EXPECT_EQ(
        (*second)[std::string_view("type")].as_string().value_or(""),
        "int");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryAuthoringApplyValidatesResultBeforePersisting)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Strict apply entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->apply_omit_document = true;
    }

    auto change = MakeStrictAuthoringChange(0);
    auto result = scheduler_->ApplyRepositoryEntryAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_FALSE((*obj)[std::string_view("ok")].as_bool().value_or(true));
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("missingField"));

    auto persisted = settings_manager_->GetTaskRepositoryEntryJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    auto authoring =
        (*persisted_obj)[std::string_view("authoring")].as_object();
    ASSERT_TRUE(authoring.has_value());
    EXPECT_EQ(
        (*authoring)[std::string_view("revision")].as_sint().value_or(-1),
        0);
    auto accepted =
        (*persisted_obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(
        (*accepted)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("default"));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryAuthoringApplyRejectsWrongDocumentRevisionWithoutPersisting)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Strict apply revision entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->apply_document_revision_delta = 1;
    }

    auto change = MakeStrictAuthoringChange(0);
    auto result = scheduler_->ApplyRepositoryEntryAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_FALSE((*obj)[std::string_view("ok")].as_bool().value_or(true));
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("revisionMismatch"));

    auto persisted = settings_manager_->GetTaskRepositoryEntryJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    auto authoring =
        (*persisted_obj)[std::string_view("authoring")].as_object();
    ASSERT_TRUE(authoring.has_value());
    EXPECT_EQ(
        (*authoring)[std::string_view("revision")].as_sint().value_or(-1),
        0);

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryAuthoringApplyPersistsStrictProviderResult)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Strict apply success entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    auto change = MakeStrictAuthoringChange(0);
    auto result = scheduler_->ApplyRepositoryEntryAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
    EXPECT_TRUE(obj->contains(std::string_view("acceptedProperties")));
    EXPECT_TRUE(obj->contains(std::string_view("document")));
    EXPECT_TRUE(obj->contains(std::string_view("diagnostics")));
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(
        (*document)[std::string_view("viewKind")].as_string().value_or(""),
        std::string_view("formSequence"));
    EXPECT_EQ(
        (*document)[std::string_view("revision")].as_sint().value_or(-1),
        1);

    auto persisted = settings_manager_->GetTaskRepositoryEntryJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    auto persisted_authoring =
        (*persisted_obj)[std::string_view("authoring")].as_object();
    ASSERT_TRUE(persisted_authoring.has_value());
    EXPECT_EQ(
        (*persisted_authoring)[std::string_view("revision")].as_sint().value_or(
            -1),
        1);
    EXPECT_EQ(
        (*persisted_authoring)[std::string_view("lastChangeKind")]
            .as_string()
            .value_or(""),
        std::string_view("setValue"));
    EXPECT_FALSE(
        persisted_authoring->contains(std::string_view("migrationState")));
    EXPECT_TRUE(
        persisted_authoring->contains(std::string_view("sourceFingerprint")));
    auto persisted_accepted =
        (*persisted_obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(persisted_accepted.has_value());
    EXPECT_EQ(
        (*persisted_accepted)[std::string_view("key1")].as_string().value_or(
            ""),
        std::string_view("accepted"));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
}

// =============================================================================
// 批量 apply 事务（changes[] 全成或全败）
// =============================================================================

// 成功组：双 change 批全部生效——revision 双推进（base+2，响应与持久层
// 一致）、每 change 各一次 ApplyChange、lastChangeKind 为 kinds 以 "+"
// 连接；持久化只在批尾发生一次（终态一次落盘）。
TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringBatchApplyCommitsAllChanges)
{
    SeedEmptyAuthoringTask(*settings_manager_, 0, 0, "initial");
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);

    auto result = scheduler_->ApplyTaskAuthoringChange(
        0,
        MakeStrictAuthoringBatchRequest(0, {"setValue", "invokeCommand"}));
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
    EXPECT_EQ((*obj)[std::string_view("revision")].as_sint().value_or(-1), 2);
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(
        (*document)[std::string_view("revision")].as_sint().value_or(-1),
        2);

    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    auto persisted_authoring =
        (*persisted_obj)[std::string_view("authoring")].as_object();
    ASSERT_TRUE(persisted_authoring.has_value());
    EXPECT_EQ(
        (*persisted_authoring)[std::string_view("revision")].as_sint().value_or(
            -1),
        2);
    EXPECT_EQ(
        (*persisted_authoring)[std::string_view("lastChangeKind")]
            .as_string()
            .value_or(""),
        std::string_view("setValue+invokeCommand"));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 2);
    EXPECT_EQ(shared_state_->authoring_session_count, 1);
}

// 顺序执行组：三 change 批、第 3 个被注入拒绝——前两个已逐个派发
// （apply_change_count==3 证明逐元素顺序执行，addGraphNode 等分配型
// change 的跨 kind 依赖依赖此顺序语义），失败定位前缀指向 change[2]，
// diagnostics 聚合携带失败 change 的注入诊断，store 零持久化。
TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringBatchApplyStopsAtFailingChange)
{
    SeedEmptyAuthoringTask(*settings_manager_, 0, 0, "initial");
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->apply_reject_at_count = 3;
    }

    auto result = scheduler_->ApplyTaskAuthoringChange(
        0,
        MakeStrictAuthoringBatchRequest(
            0,
            {"setValue", "setValue", "setValue"}));
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_FALSE((*obj)[std::string_view("ok")].as_bool().value_or(true));
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("applyRejected"));
    const auto message =
        (*obj)[std::string_view("message")].as_string().value_or("");
    EXPECT_NE(message.find("change[2](setValue)"), std::string::npos)
        << message;
    auto diagnostics = (*obj)[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(diagnostics.has_value());
    ASSERT_EQ(diagnostics->size(), 1u);

    // 中途失败零持久化：批前 revision 0 原样。
    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    auto persisted_authoring =
        (*persisted_obj)[std::string_view("authoring")].as_object();
    if (persisted_authoring.has_value())
    {
        EXPECT_EQ(
            (*persisted_authoring)[std::string_view("revision")]
                .as_sint()
                .value_or(-1),
            0);
    }

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 3);
    EXPECT_EQ(shared_state_->authoring_session_count, 1);
}

// 逐出重建组：中途失败（applyRejected）后紧跟一次合法单 change apply
// （base 仍取批前 revision）→ 成功且 revision==base+1。会话计数递增证明
// 失败批的会话缓存已被逐出、新会话从持久层（批前状态）重建——内存里
// 的半批突变从未被后续请求观察到。
TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringBatchApplyEvictsSessionAndRebuildsFromPersistedState)
{
    SeedEmptyAuthoringTask(*settings_manager_, 0, 0, "initial");
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->apply_reject_at_count = 2;
    }

    auto failed = scheduler_->ApplyTaskAuthoringChange(
        0,
        MakeStrictAuthoringBatchRequest(0, {"setValue", "setValue"}));
    auto failed_obj = failed.as_object();
    ASSERT_TRUE(failed_obj.has_value());
    EXPECT_FALSE(
        (*failed_obj)[std::string_view("ok")].as_bool().value_or(true));
    EXPECT_EQ(
        (*failed_obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("applyRejected"));

    auto recovery =
        scheduler_->ApplyTaskAuthoringChange(0, MakeStrictAuthoringChange(0));
    auto recovery_obj = recovery.as_object();
    ASSERT_TRUE(recovery_obj.has_value());
    EXPECT_TRUE(
        (*recovery_obj)[std::string_view("ok")].as_bool().value_or(false))
        << "recovery apply must succeed from the pre-batch persisted state";
    EXPECT_EQ(
        (*recovery_obj)[std::string_view("revision")].as_sint().value_or(-1),
        1);

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 3);
    EXPECT_EQ(shared_state_->authoring_session_count, 2);
}

// 中途失败回滚的 providerFailed 变体：首个 change 成功、第二个
// providerFailed——同样逐出回滚、零持久化、定位前缀指向 change[1]。
TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringBatchApplyProviderFailureRollsBackBatch)
{
    SeedEmptyAuthoringTask(*settings_manager_, 0, 0, "initial");
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->apply_fail_at_count = 2;
    }

    auto result = scheduler_->ApplyTaskAuthoringChange(
        0,
        MakeStrictAuthoringBatchRequest(0, {"setValue", "setValue"}));
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_FALSE((*obj)[std::string_view("ok")].as_bool().value_or(true));
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("providerFailed"));
    const auto message =
        (*obj)[std::string_view("message")].as_string().value_or("");
    EXPECT_NE(message.find("change[1](setValue)"), std::string::npos)
        << message;

    auto persisted = settings_manager_->GetTaskInstanceJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    auto properties =
        (*persisted_obj)[std::string_view("properties")].as_object();
    ASSERT_TRUE(properties.has_value());
    EXPECT_EQ(
        (*properties)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("initial"));

    auto recovery =
        scheduler_->ApplyTaskAuthoringChange(0, MakeStrictAuthoringChange(0));
    EXPECT_TRUE(
        (*recovery.as_object())[std::string_view("ok")].as_bool().value_or(
            false));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->authoring_session_count, 2);
}

// 首个 change 即失败：session 文档零突变，无需逐出——后续 apply 复用
// 同一会话（authoring_session_count 保持 1），回滚标志的精确性。
TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerAuthoringBatchApplyFirstChangeFailureKeepsCachedSession)
{
    SeedEmptyAuthoringTask(*settings_manager_, 0, 0, "initial");
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->apply_fail_at_count = 1;
    }

    auto result = scheduler_->ApplyTaskAuthoringChange(
        0,
        MakeStrictAuthoringBatchRequest(0, {"setValue", "setValue"}));
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("providerFailed"));
    const auto message =
        (*obj)[std::string_view("message")].as_string().value_or("");
    EXPECT_NE(message.find("change[0](setValue)"), std::string::npos)
        << message;

    auto recovery =
        scheduler_->ApplyTaskAuthoringChange(0, MakeStrictAuthoringChange(0));
    EXPECT_TRUE(
        (*recovery.as_object())[std::string_view("ok")].as_bool().value_or(
            false));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->authoring_session_count, 1);
    // 批在 change[0] 即停（1 次 ApplyChange）+ 恢复 apply（1 次）。
    EXPECT_EQ(shared_state_->apply_change_count, 2);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryCompilePreservesStrictProviderResult)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Strict compile entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    yyjson::value request(Das::Utils::MakeYyjsonObject());
    (*request.as_object())[std::string_view("purpose")] = "preview";
    auto result = scheduler_->CompileRepositoryEntryAuthoring(0, request);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ((*obj)[std::string_view("entryId")].as_sint().value_or(-1), 0);
    EXPECT_TRUE(obj->contains(std::string_view("compile")));
    EXPECT_TRUE(obj->contains(std::string_view("diagnostics")));
    auto compile = (*obj)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile.has_value());
    EXPECT_TRUE(compile->contains(std::string_view("executionInput")));
    EXPECT_FALSE(obj->contains(std::string_view("providerDebug")));
    EXPECT_FALSE(obj->contains(std::string_view("summary")));
    EXPECT_FALSE(obj->contains(std::string_view("canExecute")));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_EQ(shared_state_->get_last_compile_purpose(), "preview");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryCreateReturnsCreatedEntryJson)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // 具体类直调（COM 门面整删后）：create 响应携带新条目的完整投影
    // （entryId/displayName/acceptedProperties）。
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Bridge entry", 7));
    auto obj = created.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ((*obj)[std::string_view("entryId")].as_sint().value_or(-1), 0);
    EXPECT_EQ(
        (*obj)[std::string_view("displayName")].as_string().value_or(""),
        std::string_view("Bridge entry"));
    auto accepted = (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(
        (*accepted)[std::string_view("retryCount")].as_sint().value_or(-1),
        7);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryDeleteCreatedEntryThenMissing)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Delete me", 4));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    EXPECT_EQ(scheduler_->DeleteRepositoryEntry(0), DAS_S_OK);
    EXPECT_EQ(scheduler_->DeleteRepositoryEntry(0), DAS_E_NOT_FOUND);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryCompilePreviewProjectsProviderResult)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Compile repository entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    yyjson::value request(Das::Utils::MakeYyjsonObject());
    (*request.as_object())[std::string_view("purpose")] = "preview";
    auto result = scheduler_->CompileRepositoryEntryAuthoring(0, request);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ((*obj)[std::string_view("entryId")].as_sint().value_or(-1), 0);
    // Phase 84: Scheduler preserves the Provider Compile envelope verbatim;
    // the old canExecute/summary rewrapping is removed.
    auto compile = (*obj)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile.has_value());
    EXPECT_TRUE(
        (*compile)[std::string_view("canExecute")].as_bool().value_or(false));
    auto summary = (*compile)[std::string_view("summary")].as_object();
    ASSERT_TRUE(summary.has_value());
    auto task_names = (*summary)[std::string_view("taskNames")].as_array();
    ASSERT_TRUE(task_names.has_value());
    ASSERT_EQ(task_names->size(), 1u);
    EXPECT_EQ(
        (*task_names)[0].as_string().value_or(""),
        std::string_view("factoryTask"));
    EXPECT_FALSE(
        (*summary)[std::string_view("requiresAgentRuntime")].as_bool().value_or(
            true));

    auto diagnostics = (*obj)[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(diagnostics.has_value());
    ASSERT_EQ(diagnostics->size(), 1u);
    auto diagnostic = (*diagnostics)[0].as_object();
    ASSERT_TRUE(diagnostic.has_value());
    EXPECT_EQ(
        (*diagnostic)[std::string_view("path")].as_string().value_or(""),
        std::string_view("fake.compile"));

    EXPECT_FALSE(obj->contains(std::string_view("executionInput")));
    EXPECT_FALSE(obj->contains(std::string_view("providerDebug")));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_EQ(shared_state_->get_last_compile_purpose(), "preview");
    EXPECT_EQ(shared_state_->last_context_entry_id, 0);
    EXPECT_FALSE(shared_state_->last_context_had_task_id);
    EXPECT_EQ(shared_state_->get_last_props_key1_value(), "default");
    EXPECT_TRUE((shared_state_->executed_count == 0));
}

// graph 任务（DasGraphTask 任务类型）经真实调度器 tick 端到端执行
// componentRef 节点：OnTick 对 {"fingerprint","plan"} shell 形状的执行
// 输入直接透传（不预 cast、不重写、不诊断），shell 指纹校验由执行侧
// GraphRuntime 承担——探针组件执行即透传链路完好的证据。
TEST_F(SchedulerRuntimeBackedTest, GraphTaskShellExecutionInputPassesThrough)
{
    // FactoryPlugin 变体不带流程组件（68F1xxxx 属真实 FlowControl），执行
    // 组件指向 FakeTaskComponent 自身 guid。
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    // graph 任务实例：单 componentRef 探针节点（短字符串节点 id，节点
    // 标识统一后的生产形态）。graph 视图契约要求恰一个 Begin 边界
    // 节点（缺席即 BeginNodeMissing fatal，编译产物无执行序），预置
    // n_begin 与空图会话的播种形状一致。
    {
        yyjson::value task0(Das::Utils::MakeYyjsonObject());
        (*task0.as_object())[std::string_view("id")] = 0;
        (*task0.as_object())[std::string_view("taskGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        (*task0.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        // 立即到期：ComputeNextDelay 只看有值时间戳，null 会被排到默认
        // 长延迟（约 20s），拖垮 tick 启动。
        (*task0.as_object())[std::string_view("nextExecutionTime")] =
            static_cast<int64_t>(1);
        auto properties = Das::Utils::ParseYyjsonFromString(R"json({
            "documentId": "shell-passthrough-graph",
            "version": 1,
            "nodes": [
                {
                    "nodeId": "n_begin",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {}
                },
                {
                    "nodeId": "probe",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "97BA1BE0-A199-47CB-9604-440B8BBC6555",
                            "pluginGuid": "12345678-9ABC-4DEF-8123-456789ABCDEF"
                        }
                    },
                    "settings": {}
                }
            ],
            "edges": [],
            "tags": []
        })json");
        ASSERT_TRUE(properties.has_value());
        (*task0.as_object())[std::string_view("properties")] =
            std::move(*properties);
        WriteSchedulerState(*settings_manager_, 1, {0}, {task0});
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    bool probe_executed = false;
    {
        ipc_scoped_lock lock(shared_state_->mutex);
        probe_executed = shared_state_->cv.wait_for(
            lock,
            std::chrono::seconds(20),
            [this] { return shared_state_->component_do_count >= 1; });
    }
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
    // 让 BusinessThread 上的 OnTick 收尾（Do 被取消后其尾段仍在跑），
    // 避免 TearDown 销毁 scheduler 与 tick 尾段竞态。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT_TRUE(probe_executed)
        << "probe component never executed through the tick";

    const ipc_scoped_lock lock(shared_state_->mutex);
    // 透传不触发 Fake 会话编译：graph 任务的编译走真实 DasGraphTask
    // authoring 会话（其插件工厂），恒不经 FactoryPlugin 的 Fake——零
    // Fake/条目解析编译混入即透传链路完好的负断言证据。
    EXPECT_EQ(shared_state_->compile_count, 0);
    // 直跑任务本体（FactoryBackedTask）不应被执行——执行的是 graph 任务。
    EXPECT_EQ(shared_state_->executed_count, 0u);
}

// graph authoring 的 actionCatalog 采用 {actionId,label} 对象条目：Scheduler
// 从组件 manifest definition 的 label map 按默认 locale 解析出 label 注入
// context，经 DasGraphTask 纯管道透传由 Core 在 context 解析点播种，
// GetDocument 投影保持对象形态。断言锚定真实插件的三个组件：FlowControl
// 的 merge/launch 与 DasGraphTask 的 execute（默认 locale=en 取 map 的 en
// 值）。
TEST_F(
    SchedulerRuntimeBackedTest,
    GraphAuthoringActionCatalogCarriesManifestKindLabels)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);
    DeployRealPluginArtifacts(
        "libDasFlowControl.dll",
        "DasFlowControl.json",
        /*force_ipc=*/false,
        "DasFlowControl");

    // graph 任务实例（空属性 → 空 graph 文档）。不 Enable，只走 Get 管道。
    {
        yyjson::value task0(Das::Utils::MakeYyjsonObject());
        (*task0.as_object())[std::string_view("id")] = 0;
        (*task0.as_object())[std::string_view("taskGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        (*task0.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*task0.as_object())[std::string_view("nextExecutionTime")] =
            static_cast<int64_t>(1);
        (*task0.as_object())[std::string_view("properties")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        WriteSchedulerState(*settings_manager_, 1, {0}, {task0});
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto result = scheduler_->GetTaskAuthoringDocument(0, request);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE(obj->contains(std::string_view("document")));
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    ASSERT_TRUE(document->contains(std::string_view("actionCatalog")));
    auto catalog = (*document)[std::string_view("actionCatalog")].as_object();
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(catalog->contains(std::string_view("entries")));
    auto entries = (*catalog)[std::string_view("entries")].as_array();
    ASSERT_TRUE(entries.has_value());

    // label 期望 = manifest definition label map 的 en 值（默认 locale=en）；
    // 无 label map 的探针组件回退 kind 字符串（业务中立：调度器只解析
    // manifest 数据，不识别任何前缀）。
    const std::vector<std::pair<std::string_view, std::string_view>> expected =
        {
            {"68F10008-0000-4000-8000-000000000008", "Merge"},
            {"95FD2FFA-88F2-4F85-A536-C708D25B641F", "Launch Process"},
            {"F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3", "Execute Graph"},
            {FactoryTaskComponentImplGuidString, "test.execution"},
        };
    for (const auto& [action_id, label] : expected)
    {
        bool found = false;
        for (const auto& entry : *entries)
        {
            auto entry_obj = entry.as_object();
            if (!entry_obj
                || !entry_obj->contains(std::string_view("actionId")))
            {
                continue;
            }
            auto id = (*entry_obj)[std::string_view("actionId")].as_string();
            if (!id || *id != action_id)
            {
                continue;
            }
            found = true;
            ASSERT_TRUE(entry_obj->contains(std::string_view("label")));
            EXPECT_EQ(
                (*entry_obj)[std::string_view("label")].as_string().value_or(
                    ""),
                label);
        }
        EXPECT_TRUE(found) << "actionId missing from catalog: " << action_id;
    }
}

// manifest required 声明全链透传：真实 DasFlowControl manifest 的 launch
// exePath required:true 经 AppendConfigFromDefinition 发射 → catalog 通道
// （context actionCatalog 键集）→ 会话投影落到 addGraphNode 响应的节点
// fields[].required；同组件未声明的字段（args）保持 false。断言锚定真实
// 插件部署（非手工 catalog 注入）——任何一层漏透传该断言即失败。
TEST_F(
    SchedulerRuntimeBackedTest,
    GraphAuthoringManifestRequiredProjectsIntoNodeFields)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);
    DeployRealPluginArtifacts(
        "libDasFlowControl.dll",
        "DasFlowControl.json",
        /*force_ipc=*/false,
        "DasFlowControl");

    // graph 任务实例（空属性 → 空 graph 文档）。不 Enable，只走 apply 管线。
    {
        yyjson::value task0(Das::Utils::MakeYyjsonObject());
        (*task0.as_object())[std::string_view("id")] = 0;
        (*task0.as_object())[std::string_view("taskGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        (*task0.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*task0.as_object())[std::string_view("nextExecutionTime")] =
            static_cast<int64_t>(1);
        (*task0.as_object())[std::string_view("properties")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        WriteSchedulerState(*settings_manager_, 1, {0}, {task0});
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    yyjson::value change(Das::Utils::MakeYyjsonObject());
    (*change.as_object())[std::string_view("baseRevision")] =
        static_cast<int64_t>(0);
    yyjson::value element(Das::Utils::MakeYyjsonObject());
    (*element.as_object())[std::string_view("kind")] =
        std::make_pair(std::string_view("addGraphNode"), yyjson::copy_string);
    yyjson::value payload(Das::Utils::MakeYyjsonObject());
    (*payload.as_object())[std::string_view("actionId")] =
        "95FD2FFA-88F2-4F85-A536-C708D25B641F";
    (*element.as_object())[std::string_view("payload")] = std::move(payload);
    auto changes = Das::Utils::MakeYyjsonArray();
    (*changes.as_array()).emplace_back(std::move(element));
    (*change.as_object())[std::string_view("changes")] = std::move(changes);

    auto result = scheduler_->ApplyTaskAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    ASSERT_TRUE(document->contains(std::string_view("graph")));
    auto graph = (*document)[std::string_view("graph")].as_object();
    ASSERT_TRUE(graph.has_value());
    ASSERT_TRUE(graph->contains(std::string_view("nodes")));
    auto nodes = (*graph)[std::string_view("nodes")].as_array();
    ASSERT_TRUE(nodes.has_value());
    // 空图会话预置 begin/end（n_0/n_1）两边界节点，addGraphNode 之后
    // 共 3 个；action 节点按 componentGuid 定位，不依赖节点位置。
    ASSERT_EQ(nodes->size(), 3u);
    yyjson::value action_node(Das::Utils::MakeYyjsonObject());
    bool          action_found = false;
    for (const auto& node : *nodes)
    {
        auto candidate = node.as_object();
        if (!candidate)
        {
            continue;
        }
        const auto component_guid =
            (*candidate)[std::string_view("componentGuid")]
                .as_string()
                .value_or("");
        if (component_guid
            == std::string_view("95FD2FFA-88F2-4F85-A536-C708D25B641F"))
        {
            action_node = Das::Utils::CloneYyjsonValue(node);
            action_found = true;
            break;
        }
    }
    ASSERT_TRUE(action_found);
    auto node_obj = action_node.as_object();
    ASSERT_TRUE(node_obj.has_value());
    ASSERT_TRUE(node_obj->contains(std::string_view("fields")));
    auto fields = (*node_obj)[std::string_view("fields")].as_array();
    ASSERT_TRUE(fields.has_value());
    bool exe_path_found = false;
    bool args_found = false;
    for (const auto& field : *fields)
    {
        auto field_obj = field.as_object();
        if (!field_obj || !field_obj->contains(std::string_view("name")))
        {
            continue;
        }
        const auto name =
            (*field_obj)[std::string_view("name")].as_string().value_or("");
        if (name == std::string_view("exePath"))
        {
            exe_path_found = true;
            ASSERT_TRUE(field_obj->contains(std::string_view("required")));
            EXPECT_TRUE(
                (*field_obj)[std::string_view("required")].as_bool().value_or(
                    false));
        }
        else if (name == std::string_view("args"))
        {
            args_found = true;
            ASSERT_TRUE(field_obj->contains(std::string_view("required")));
            EXPECT_FALSE(
                (*field_obj)[std::string_view("required")].as_bool().value_or(
                    true));
        }
    }
    EXPECT_TRUE(exe_path_found) << "exePath field missing from projection";
    EXPECT_TRUE(args_found) << "args field missing from projection";
}

namespace
{
    // 预备 pass 用例的共用：构造一个 graph 任务实例（raw GraphDocumentDto
    // 形状 properties 直书）并写入 settings——不 Enable，只走 Get/Apply
    // authoring 管线。properties_json 为图文档原文。
    void SeedGraphTaskInstance(
        Das::Core::SettingsManager::SettingsManager& settings,
        yyjson::value                                properties)
    {
        yyjson::value task0(Das::Utils::MakeYyjsonObject());
        (*task0.as_object())[std::string_view("id")] = 0;
        (*task0.as_object())[std::string_view("taskGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        (*task0.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*task0.as_object())[std::string_view("nextExecutionTime")] =
            static_cast<int64_t>(1);
        (*task0.as_object())[std::string_view("properties")] =
            std::move(properties);
        WriteSchedulerState(settings, 1, {0}, {task0});
    }

    // 图文档 Get 投影中按 componentGuid 定位节点（返回克隆值）；找不到
    // 返回 disengaged。
    std::optional<yyjson::value> FindGraphNodeByComponent(
        const yyjson::value& document,
        std::string_view     component_guid)
    {
        auto doc_obj = document.as_object();
        if (!doc_obj || !doc_obj->contains(std::string_view("graph")))
        {
            return std::nullopt;
        }
        auto graph = (*doc_obj)[std::string_view("graph")].as_object();
        if (!graph || !graph->contains(std::string_view("nodes")))
        {
            return std::nullopt;
        }
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        if (!nodes)
        {
            return std::nullopt;
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (!node_obj
                || !node_obj->contains(std::string_view("componentGuid")))
            {
                continue;
            }
            auto guid =
                (*node_obj)[std::string_view("componentGuid")].as_string();
            if (guid && *guid == component_guid)
            {
                return Das::Utils::CloneYyjsonValue(node);
            }
        }
        return std::nullopt;
    }

    // 节点投影 ports[] 中是否存在指定 portId。
    bool NodeProjectsPort(const yyjson::value& node, std::string_view port_id)
    {
        auto node_obj = node.as_object();
        if (!node_obj || !node_obj->contains(std::string_view("ports")))
        {
            return false;
        }
        auto ports = (*node_obj)[std::string_view("ports")].as_array();
        if (!ports)
        {
            return false;
        }
        for (const auto& port : *ports)
        {
            auto port_obj = port.as_object();
            if (!port_obj || !port_obj->contains(std::string_view("portId")))
            {
                continue;
            }
            auto id = (*port_obj)[std::string_view("portId")].as_string();
            if (id && *id == port_id)
            {
                return true;
            }
        }
        return false;
    }
} // namespace

// 预备 pass·nodeFaces 分支：begin 边界节点的 settings.boundaryPorts 经
// 注册面反查（FindByExecutionComponentGuid）命中既有 QI 链，边界会话
// GetDocument 现算口面注入图会话 context，Get 投影的该节点 ports[] 出现
// 动态口（p1/string/input）。end 节点口为 output。纯 Get 零写回：两次
// Get 之间任务存储文件内容逐字节不变。
TEST_F(
    SchedulerRuntimeBackedTest,
    GraphFacesPassInjectsNodeFacesForBoundaryNodes)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    {
        auto properties = Das::Utils::ParseYyjsonFromString(R"json({
            "documentId": "faces-node-graph",
            "version": 1,
            "nodes": [
                {
                    "nodeId": "n_begin",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "boundaryPorts": [
                            {"id": "p1", "type": "string"}
                        ]
                    }
                },
                {
                    "nodeId": "n_end",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "boundaryPorts": [
                            {"id": "q1", "type": "int"}
                        ]
                    }
                }
            ],
            "edges": [],
            "tags": []
        })json");
        ASSERT_TRUE(properties.has_value());
        SeedGraphTaskInstance(*settings_manager_, std::move(*properties));
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const auto task_json_before =
        settings_manager_->GetTaskInstanceJson("0", 0);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto result = scheduler_->GetTaskAuthoringDocument(0, request);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE(obj->contains(std::string_view("document")));
    const auto& document = (*obj)[std::string_view("document")];

    auto begin_node = FindGraphNodeByComponent(
        document,
        Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID);
    ASSERT_TRUE(begin_node.has_value())
        << "begin node missing from graph projection";
    EXPECT_TRUE(NodeProjectsPort(*begin_node, "p1"))
        << "begin node face port p1 missing from projection";

    auto end_node = FindGraphNodeByComponent(
        document,
        Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID);
    ASSERT_TRUE(end_node.has_value())
        << "end node missing from graph projection";
    EXPECT_TRUE(NodeProjectsPort(*end_node, "q1"))
        << "end node face port q1 missing from projection";

    // 纯 Get 零写回：存储任务实例逐字节不变。
    const auto task_json_after = settings_manager_->GetTaskInstanceJson("0", 0);
    EXPECT_EQ(
        Das::Utils::SerializeYyjsonValue(task_json_before, true).value_or(""),
        Das::Utils::SerializeYyjsonValue(task_json_after, true).value_or(""));
}

// 预备 pass·entryFaces 分支：绑条目调用节点（taskRepositoryRef 字段持正
// 整数）取条目缓存会话 GetDocument 现算口面注入；条目文档无 ports section
// 时跳过（零 face 零诊断）；条目 revision 推进后下一次宿主 Get 重建会话
// 取到新 face（无跨文档陈旧）。
TEST_F(SchedulerRuntimeBackedTest, GraphFacesPassInjectsEntryFacesForBoundNode)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);
    DeployRealPluginArtifacts(
        "libDasFlowControl.dll",
        "DasFlowControl.json",
        /*force_ipc=*/false,
        "DasFlowControl");

    // 绑定目标条目：FactoryTask 类型（Fake 会话供源），entry id 7。
    SeedRepositoryEntry(
        *settings_manager_,
        7,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        "Bound entry");

    {
        auto properties = Das::Utils::ParseYyjsonFromString(R"json({
            "documentId": "faces-entry-graph",
            "version": 1,
            "nodes": [
                {
                    "nodeId": "n_begin",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {}
                },
                {
                    "nodeId": "invoke",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "repositoryRef": 7
                    }
                }
            ],
            "edges": [],
            "tags": []
        })json");
        ASSERT_TRUE(properties.has_value());
        SeedGraphTaskInstance(*settings_manager_, std::move(*properties));
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Fake 会话走严格 envelope 形态（ports section 探针挂在该形态上）。
    EnableStrictAuthoringWire(*shared_state_);

    const auto invoke_node_guid =
        std::string_view("F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3");
    const yyjson::value request(Das::Utils::MakeYyjsonObject());

    // ① 条目文档无 ports section（A4）：跳过——绑节点无动态口，Get 正常。
    {
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")));
        auto node = FindGraphNodeByComponent(
            (*obj)[std::string_view("document")],
            invoke_node_guid);
        ASSERT_TRUE(node.has_value());
        EXPECT_FALSE(NodeProjectsPort(*node, "e1"))
            << "entry face leaked while the entry document carries no ports "
               "section";
    }

    // ② 开 ports section（id=e1）并推进条目 revision（对条目本身 apply——
    // face 内容变化必伴随条目 revision 推进，生产语义）→ 下一次宿主 Get
    // 注入该 face，且条目会话因 revision 失效重建。
    int64_t entry_revision_now = 5;
    int     session_count_after_first_face = 0;
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->emit_ports_section = true;
        shared_state_->set_emit_ports_section_id("e1");
    }
    {
        auto entry_apply = scheduler_->ApplyRepositoryEntryAuthoringChange(
            7,
            MakeRepositoryAuthoringChange(5));
        auto entry_obj = entry_apply.as_object();
        ASSERT_TRUE(entry_obj.has_value());
        EXPECT_TRUE(
            (*entry_obj)[std::string_view("ok")].as_bool().value_or(false));
        entry_revision_now =
            (*entry_obj)[std::string_view("revision")].as_sint().value_or(5);
        EXPECT_EQ(entry_revision_now, 6);
    }
    {
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")));
        auto node = FindGraphNodeByComponent(
            (*obj)[std::string_view("document")],
            invoke_node_guid);
        ASSERT_TRUE(node.has_value());
        EXPECT_TRUE(NodeProjectsPort(*node, "e1"))
            << "bound node face port e1 missing from projection";
        const ipc_scoped_lock lock(shared_state_->mutex);
        session_count_after_first_face = shared_state_->authoring_session_count;
    }

    // ③ face 内容再变（id=e2）+ 条目 revision 再推进 → 下一次宿主 Get 取到
    // 新 face——无跨文档陈旧（宿主图会话随条目 revision 戳失效重建）。
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->set_emit_ports_section_id("e2");
    }
    {
        auto entry_apply = scheduler_->ApplyRepositoryEntryAuthoringChange(
            7,
            MakeRepositoryAuthoringChange(entry_revision_now));
        auto entry_obj = entry_apply.as_object();
        ASSERT_TRUE(entry_obj.has_value());
        EXPECT_TRUE(
            (*entry_obj)[std::string_view("ok")].as_bool().value_or(false));
    }
    {
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")));
        auto node = FindGraphNodeByComponent(
            (*obj)[std::string_view("document")],
            invoke_node_guid);
        ASSERT_TRUE(node.has_value());
        EXPECT_FALSE(NodeProjectsPort(*node, "e1"))
            << "stale entry face survived the entry revision advance";
        EXPECT_TRUE(NodeProjectsPort(*node, "e2"))
            << "fresh entry face missing after the entry revision advance";
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_GT(
            shared_state_->authoring_session_count,
            session_count_after_first_face)
            << "entry session was not rebuilt after its revision advanced";
    }
}

// 预备 pass·图条目自述口面（graph→graph 嵌套全链）：内层图条目（真实
// DasGraphTask 会话，begin/end 带动态口）经条目 Get 注入自己的 faces 后，
// 图视图投影根 ports section=推导边界（begin→input/end→output）——预备
// pass 的绑条目抽取路径零改动即可命中（ExtractFacePortsValue 读根
// ports section），父图调用节点口面含内层动态口，连线 Apply（faces 通道
// 端点解析）成功。纯 Get 零写回：条目存储逐字节不变。
TEST_F(
    SchedulerRuntimeBackedTest,
    NestedGraphEntrySelfDescribesBoundaryThroughProjection)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);
    DeployRealPluginArtifacts(
        "libDasFlowControl.dll",
        "DasFlowControl.json",
        /*force_ipc=*/false,
        "DasFlowControl");

    // 内层图条目（entry 7）：begin p1（string）/end q1（int）动态口，无绑
    // 条目（混合 revision 戳与条目自身 revision 一致——预备 pass 复用带
    // faces 的缓存条目会话）。
    {
        yyjson::value entry(Das::Utils::MakeYyjsonObject());
        (*entry.as_object())[std::string_view("entryId")] =
            static_cast<int64_t>(7);
        (*entry.as_object())[std::string_view("displayName")] =
            "Nested graph entry";
        (*entry.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*entry.as_object())[std::string_view("taskTypeGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        {
            yyjson::value authoring(Das::Utils::MakeYyjsonObject());
            (*authoring.as_object())[std::string_view("revision")] = 5;
            (*authoring.as_object())[std::string_view("lastChangeKind")] =
                std::make_pair(
                    std::string_view("setValue"),
                    yyjson::copy_string);
            (*authoring.as_object())[std::string_view("sourceFingerprint")] =
                std::make_pair(
                    std::string_view("nested-seed"),
                    yyjson::copy_string);
            (*entry.as_object())[std::string_view("authoring")] =
                std::move(authoring);
        }
        auto inner_properties = Das::Utils::ParseYyjsonFromString(R"json({
            "documentId": "nested-inner",
            "version": 1,
            "nodes": [
                {
                    "nodeId": "inner_begin",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "boundaryPorts": [
                            {"id": "p1", "type": "string"}
                        ]
                    }
                },
                {
                    "nodeId": "inner_end",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "boundaryPorts": [
                            {"id": "q1", "type": "int"}
                        ]
                    }
                }
            ],
            "edges": [],
            "tags": []
        })json");
        ASSERT_TRUE(inner_properties.has_value());
        (*entry.as_object())[std::string_view("acceptedProperties")] =
            std::move(*inner_properties);
        (*entry.as_object())[std::string_view("graphDocument")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        {
            yyjson::value availability(Das::Utils::MakeYyjsonObject());
            (*availability.as_object())[std::string_view("state")] =
                "available";
            (*entry.as_object())[std::string_view("availability")] =
                std::move(availability);
        }
        ASSERT_EQ(
            settings_manager_->UpdateTaskRepositoryEntryJson("0", 7, entry),
            DAS_S_OK);
    }

    // 父图任务实例（task 0）：begin 动态口 p0（连线源——begin 口对的出侧
    // 供图内寻址）+ 绑条目调用节点（GraphTask 执行组件，repositoryRef
    // 持 entry 7）。
    {
        auto properties = Das::Utils::ParseYyjsonFromString(R"json({
            "documentId": "nested-parent",
            "version": 1,
            "nodes": [
                {
                    "nodeId": "n_begin",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "boundaryPorts": [
                            {"id": "p0", "type": "string"}
                        ]
                    }
                },
                {
                    "nodeId": "invoke",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "repositoryRef": 7
                    }
                }
            ],
            "edges": [],
            "tags": []
        })json");
        ASSERT_TRUE(properties.has_value());
        // authoring 域 revision 与文档 version 同步（生产不变量：每次
        // apply 两域同步推进——连线 Apply 的 baseRevision 墙按 authoring
        // 域判定）。
        yyjson::value task0(Das::Utils::MakeYyjsonObject());
        (*task0.as_object())[std::string_view("id")] = 0;
        (*task0.as_object())[std::string_view("taskGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        (*task0.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*task0.as_object())[std::string_view("nextExecutionTime")] =
            static_cast<int64_t>(1);
        {
            yyjson::value authoring(Das::Utils::MakeYyjsonObject());
            (*authoring.as_object())[std::string_view("revision")] = 1;
            (*task0.as_object())[std::string_view("authoring")] =
                std::move(authoring);
        }
        (*task0.as_object())[std::string_view("properties")] =
            std::move(*properties);
        WriteSchedulerState(*settings_manager_, 1, {0}, {task0});
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const auto entry_json_before =
        settings_manager_->GetTaskRepositoryEntryJson("0", 7);

    // ① 内层图条目 Get：预备 pass 注入 begin/end faces → 图视图投影根
    //    ports section=推导边界（p1→input / q1→output）——文档自述口面。
    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    {
        auto result =
            scheduler_->GetRepositoryEntryAuthoringDocument(7, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")))
            << "entry Get failed: "
            << Das::Utils::SerializeYyjsonValue(result).value_or("");
        const auto& document = (*obj)[std::string_view("document")];
        auto        doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        ASSERT_TRUE(doc_obj->contains(std::string_view("ports")))
            << "inner graph entry document carries no root ports section";
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports.has_value());
        ASSERT_EQ(ports->size(), 2u);
        auto first = (*ports)[0].as_object();
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(
            (*first)[std::string_view("id")].as_string().value_or(""),
            "p1");
        EXPECT_EQ(
            (*first)[std::string_view("direction")].as_string().value_or(""),
            "input");
        EXPECT_EQ(
            (*first)[std::string_view("type")].as_string().value_or(""),
            "string");
        auto second = (*ports)[1].as_object();
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(
            (*second)[std::string_view("id")].as_string().value_or(""),
            "q1");
        EXPECT_EQ(
            (*second)[std::string_view("direction")].as_string().value_or(""),
            "output");
    }

    // 纯 Get 零写回：条目存储逐字节不变（ports section 是投影读通道，
    // 不落 acceptedProperties）。
    {
        const auto entry_json_after =
            settings_manager_->GetTaskRepositoryEntryJson("0", 7);
        EXPECT_EQ(
            Das::Utils::SerializeYyjsonValue(entry_json_before, true)
                .value_or(""),
            Das::Utils::SerializeYyjsonValue(entry_json_after, true)
                .value_or(""));
    }

    // ② 父图 Get：预备 pass 绑条目分支复用带 faces 的条目缓存会话 →
    //    GetDocument 根 ports section 被抽取路径命中 → 调用节点口面含
    //    内层动态口（p1/q1）。
    const auto invoke_node_guid =
        std::string_view("F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3");
    {
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")));
        auto node = FindGraphNodeByComponent(
            (*obj)[std::string_view("document")],
            invoke_node_guid);
        ASSERT_TRUE(node.has_value())
            << "bound call node missing from the parent projection";
        EXPECT_TRUE(NodeProjectsPort(*node, "p1"))
            << "inner begin dynamic port p1 missing from the call node face";
        EXPECT_TRUE(NodeProjectsPort(*node, "q1"))
            << "inner end dynamic port q1 missing from the call node face";
    }

    // ③ 连线 Apply（faces 通道端点解析）：begin 动态口 p0（出侧）→ 调用
    //    节点 p1（entryFaces 口）——受理且边落盘。baseRevision=authoring
    //    域 revision（seed 与文档 version 同步为 1）。
    {
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")));
        auto doc_obj = (*obj)[std::string_view("document")].as_object();
        ASSERT_TRUE(doc_obj.has_value());
        const auto revision =
            (*doc_obj)[std::string_view("revision")].as_sint().value_or(1);

        yyjson::value change(Das::Utils::MakeYyjsonObject());
        (*change.as_object())[std::string_view("baseRevision")] = revision;
        yyjson::value element(Das::Utils::MakeYyjsonObject());
        (*element.as_object())[std::string_view("kind")] = std::make_pair(
            std::string_view("connectGraphPorts"),
            yyjson::copy_string);
        yyjson::value payload(Das::Utils::MakeYyjsonObject());
        (*payload.as_object())[std::string_view("fromNodeId")] =
            std::make_pair(std::string_view("n_begin"), yyjson::copy_string);
        (*payload.as_object())[std::string_view("fromPortId")] =
            std::make_pair(std::string_view("p0"), yyjson::copy_string);
        (*payload.as_object())[std::string_view("toNodeId")] =
            std::make_pair(std::string_view("invoke"), yyjson::copy_string);
        (*payload.as_object())[std::string_view("toPortId")] =
            std::make_pair(std::string_view("p1"), yyjson::copy_string);
        (*element.as_object())[std::string_view("payload")] =
            std::move(payload);
        auto changes = Das::Utils::MakeYyjsonArray();
        (*changes.as_array()).emplace_back(std::move(element));
        (*change.as_object())[std::string_view("changes")] = std::move(changes);

        auto apply = scheduler_->ApplyTaskAuthoringChange(0, change);
        auto apply_obj = apply.as_object();
        ASSERT_TRUE(apply_obj.has_value());
        EXPECT_TRUE(
            (*apply_obj)[std::string_view("ok")].as_bool().value_or(false))
            << "connect onto the inner dynamic port via entryFaces was "
               "rejected: "
            << Das::Utils::SerializeYyjsonValue(apply).value_or("");

        auto result_after = scheduler_->GetTaskAuthoringDocument(0, request);
        auto after_obj = result_after.as_object();
        ASSERT_TRUE(after_obj.has_value());
        ASSERT_TRUE(after_obj->contains(std::string_view("document")));
        auto after_doc = (*after_obj)[std::string_view("document")].as_object();
        ASSERT_TRUE(after_doc.has_value());
        ASSERT_TRUE(after_doc->contains(std::string_view("graph")));
        auto graph = (*after_doc)[std::string_view("graph")].as_object();
        ASSERT_TRUE(graph.has_value());
        ASSERT_TRUE(graph->contains(std::string_view("connections")));
        auto connections = (*graph)[std::string_view("connections")].as_array();
        ASSERT_TRUE(connections.has_value());
        bool edge_found = false;
        for (const auto& connection : *connections)
        {
            auto conn_obj = connection.as_object();
            if (!conn_obj
                || (*conn_obj)[std::string_view("fromNodeId")]
                           .as_string()
                           .value_or("")
                       != std::string_view("n_begin")
                || (*conn_obj)[std::string_view("fromPortId")]
                           .as_string()
                           .value_or("")
                       != std::string_view("p0")
                || (*conn_obj)[std::string_view("toNodeId")]
                           .as_string()
                           .value_or("")
                       != std::string_view("invoke")
                || (*conn_obj)[std::string_view("toPortId")]
                           .as_string()
                           .value_or("")
                       != std::string_view("p1"))
            {
                continue;
            }
            edge_found = true;
        }
        EXPECT_TRUE(edge_found)
            << "the accepted connect did not persist into the document";
    }
}

// 多值边界输出 E2E·嵌套半（graph→graph）：内层图条目 begin 1 输入（b_1）
// + end 2 输出（e_1 int / e_2 string）——条目 Get 根 ports section 三口全
// 量可见（无跳过）、父图预备 pass 抽取命中（调用节点口面含三口）、两条
// 下游边经 faces 端点解析 Apply 落盘；子图执行编译产物（purpose=execution，
// 与调用节点的条目解析同通道——creator 会话编译）的 boundaryPorts.outputs
// 携带 e_1/e_2 双口清单（执行期 typed 边界导出与调用节点逐口投影的消费清
// 单）。注：调度器接口面（原手写 COM 接口头，无 IDL 无 IPC）已整删，
// worker 子进程经具名服务解析仓库条目的通路随之从根上消亡；父图经
// RunTaskNow 的执行半段由 MultiValueBoundaryOutputs 任务级用例与
// GraphRuntime 层 invoke 投影用例分段实测。
TEST_F(
    SchedulerRuntimeBackedTest,
    NestedGraphMultiValueOutputsFlowToDownstreamNodes)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    // 下游收值探针的口面（编译侧边校验需要声明口）。
    InjectPortDeclarationsIntoFactoryManifest(
        manifest_path_,
        FactoryTaskComponentImplGuidString,
        R"json({
            "inputs": [
                {"id": "in_int", "type": "int"},
                {"id": "in_string", "type": "string"}
            ]
        })json");
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);
    DeployRealPluginArtifacts(
        "libDasFlowControl.dll",
        "DasFlowControl.json",
        /*force_ipc=*/false,
        "DasFlowControl");

    // 内层图条目（entry 7）：begin b_1（string 入）+ 常数供值节点 +
    // end e_1（int）/e_2（string）——两条内部数据边把常数送进 end 动态口。
    {
        yyjson::value entry(Das::Utils::MakeYyjsonObject());
        (*entry.as_object())[std::string_view("entryId")] =
            static_cast<int64_t>(7);
        (*entry.as_object())[std::string_view("displayName")] =
            "Nested multi-value graph entry";
        (*entry.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*entry.as_object())[std::string_view("taskTypeGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        {
            yyjson::value authoring(Das::Utils::MakeYyjsonObject());
            (*authoring.as_object())[std::string_view("revision")] = 5;
            (*authoring.as_object())[std::string_view("lastChangeKind")] =
                std::make_pair(
                    std::string_view("setValue"),
                    yyjson::copy_string);
            (*authoring.as_object())[std::string_view("sourceFingerprint")] =
                std::make_pair(
                    std::string_view("nested-multi-seed"),
                    yyjson::copy_string);
            (*entry.as_object())[std::string_view("authoring")] =
                std::move(authoring);
        }
        auto inner_properties = Das::Utils::ParseYyjsonFromString(R"json({
            "documentId": "nested-multi-inner",
            "version": 1,
            "nodes": [
                {
                    "nodeId": "inner_begin",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "boundaryPorts": [
                            {"id": "b_1", "type": "string"}
                        ]
                    }
                },
                {
                    "nodeId": "inner_int",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "68F1000A-0000-4000-8000-00000000000A",
                            "pluginGuid": "7115BBFA-7E57-494E-82D5-D4134C367E13"
                        }
                    },
                    "settings": {
                        "value": 4242
                    }
                },
                {
                    "nodeId": "inner_str",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "68F1000C-0000-4000-8000-00000000000C",
                            "pluginGuid": "7115BBFA-7E57-494E-82D5-D4134C367E13"
                        }
                    },
                    "settings": {
                        "value": "multi-out"
                    }
                },
                {
                    "nodeId": "inner_end",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "boundaryPorts": [
                            {"id": "e_1", "type": "int"},
                            {"id": "e_2", "type": "string"}
                        ]
                    }
                }
            ],
            "edges": [
                {
                    "edgeId": "inner_e_int",
                    "sourceNodeId": "inner_int",
                    "sourcePortId": "value",
                    "targetNodeId": "inner_end",
                    "targetPortId": "e_1",
                    "edgeType": "data"
                },
                {
                    "edgeId": "inner_e_str",
                    "sourceNodeId": "inner_str",
                    "sourcePortId": "value",
                    "targetNodeId": "inner_end",
                    "targetPortId": "e_2",
                    "edgeType": "data"
                }
            ],
            "tags": []
        })json");
        ASSERT_TRUE(inner_properties.has_value());
        (*entry.as_object())[std::string_view("acceptedProperties")] =
            std::move(*inner_properties);
        (*entry.as_object())[std::string_view("graphDocument")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        {
            yyjson::value availability(Das::Utils::MakeYyjsonObject());
            (*availability.as_object())[std::string_view("state")] =
                "available";
            (*entry.as_object())[std::string_view("availability")] =
                std::move(availability);
        }
        ASSERT_EQ(
            settings_manager_->UpdateTaskRepositoryEntryJson("0", 7, entry),
            DAS_S_OK);
    }

    // 父图任务实例（task 0）：begin + 调用节点（绑 entry 7）+ 两个下游收
    // 值探针（in_int / in_string）。
    {
        auto properties = Das::Utils::ParseYyjsonFromString(R"json({
            "documentId": "nested-multi-parent",
            "version": 1,
            "nodes": [
                {
                    "nodeId": "n_begin",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {}
                },
                {
                    "nodeId": "invoke",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "repositoryRef": 7
                    }
                },
                {
                    "nodeId": "sink_int",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "97BA1BE0-A199-47CB-9604-440B8BBC6555",
                            "pluginGuid": "12345678-9ABC-4DEF-8123-456789ABCDEF"
                        }
                    },
                    "settings": {}
                },
                {
                    "nodeId": "sink_str",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "97BA1BE0-A199-47CB-9604-440B8BBC6555",
                            "pluginGuid": "12345678-9ABC-4DEF-8123-456789ABCDEF"
                        }
                    },
                    "settings": {}
                }
            ],
            "edges": [],
            "tags": []
        })json");
        ASSERT_TRUE(properties.has_value());
        // authoring 域 revision 与文档 version 同步（连线 Apply 的
        // baseRevision 墙按 authoring 域判定）。
        yyjson::value task0(Das::Utils::MakeYyjsonObject());
        (*task0.as_object())[std::string_view("id")] = 0;
        (*task0.as_object())[std::string_view("taskGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        (*task0.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*task0.as_object())[std::string_view("nextExecutionTime")] =
            static_cast<int64_t>(1);
        {
            yyjson::value authoring(Das::Utils::MakeYyjsonObject());
            (*authoring.as_object())[std::string_view("revision")] = 1;
            (*task0.as_object())[std::string_view("authoring")] =
                std::move(authoring);
        }
        (*task0.as_object())[std::string_view("properties")] =
            std::move(*properties);
        WriteSchedulerState(*settings_manager_, 1, {0}, {task0});
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());

    // ① 内层图条目 Get：根 ports section 三口全量（无跳过）——文档自述
    //    被调面。
    {
        auto result =
            scheduler_->GetRepositoryEntryAuthoringDocument(7, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")))
            << "entry Get failed: "
            << Das::Utils::SerializeYyjsonValue(result).value_or("");
        auto doc_obj = (*obj)[std::string_view("document")].as_object();
        ASSERT_TRUE(doc_obj.has_value());
        ASSERT_TRUE(doc_obj->contains(std::string_view("ports")))
            << "inner graph entry document carries no root ports section";
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports.has_value());
        ASSERT_EQ(ports->size(), 3u);
        const auto port_of = [&ports](std::size_t index)
        { return (*ports)[index].as_object(); };
        ASSERT_TRUE(port_of(0).has_value());
        EXPECT_EQ(
            (*port_of(0))[std::string_view("id")].as_string().value_or(""),
            "b_1");
        EXPECT_EQ(
            (*port_of(0))[std::string_view("direction")].as_string().value_or(
                ""),
            "input");
        ASSERT_TRUE(port_of(1).has_value());
        EXPECT_EQ(
            (*port_of(1))[std::string_view("id")].as_string().value_or(""),
            "e_1");
        EXPECT_EQ(
            (*port_of(1))[std::string_view("type")].as_string().value_or(""),
            "int");
        EXPECT_EQ(
            (*port_of(1))[std::string_view("direction")].as_string().value_or(
                ""),
            "output");
        ASSERT_TRUE(port_of(2).has_value());
        EXPECT_EQ(
            (*port_of(2))[std::string_view("id")].as_string().value_or(""),
            "e_2");
        EXPECT_EQ(
            (*port_of(2))[std::string_view("type")].as_string().value_or(""),
            "string");
        EXPECT_EQ(
            (*port_of(2))[std::string_view("direction")].as_string().value_or(
                ""),
            "output");
    }

    // ② 父图 Get：预备 pass 抽取命中——调用节点口面含三口。
    const auto invoke_node_guid =
        std::string_view("F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3");
    {
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")));
        auto node = FindGraphNodeByComponent(
            (*obj)[std::string_view("document")],
            invoke_node_guid);
        ASSERT_TRUE(node.has_value())
            << "bound call node missing from the parent projection";
        EXPECT_TRUE(NodeProjectsPort(*node, "b_1"))
            << "inner begin dynamic port b_1 missing from the call node face";
        EXPECT_TRUE(NodeProjectsPort(*node, "e_1"))
            << "inner end dynamic port e_1 missing from the call node face";
        EXPECT_TRUE(NodeProjectsPort(*node, "e_2"))
            << "inner end dynamic port e_2 missing from the call node face";
    }

    // ③ 两条下游边 Apply（faces 端点解析）：invoke.e_1→sink_int.in_int /
    //    invoke.e_2→sink_str.in_string——单批受理且双双落盘。
    {
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")));
        auto doc_obj = (*obj)[std::string_view("document")].as_object();
        ASSERT_TRUE(doc_obj.has_value());
        const auto revision =
            (*doc_obj)[std::string_view("revision")].as_sint().value_or(1);

        const auto make_connect_element = [](std::string_view from_port,
                                             std::string_view to_node,
                                             std::string_view to_port)
        {
            // 批量信封条目只带 kind+payload（baseRevision 属外层信封键）。
            yyjson::value element(Das::Utils::MakeYyjsonObject());
            (*element.as_object())[std::string_view("kind")] = std::make_pair(
                std::string_view("connectGraphPorts"),
                yyjson::copy_string);
            yyjson::value payload(Das::Utils::MakeYyjsonObject());
            (*payload.as_object())[std::string_view("fromNodeId")] =
                std::make_pair(std::string_view("invoke"), yyjson::copy_string);
            (*payload.as_object())[std::string_view("fromPortId")] =
                std::make_pair(from_port, yyjson::copy_string);
            (*payload.as_object())[std::string_view("toNodeId")] =
                std::make_pair(to_node, yyjson::copy_string);
            (*payload.as_object())[std::string_view("toPortId")] =
                std::make_pair(to_port, yyjson::copy_string);
            (*element.as_object())[std::string_view("payload")] =
                std::move(payload);
            return element;
        };
        yyjson::value change(Das::Utils::MakeYyjsonObject());
        (*change.as_object())[std::string_view("baseRevision")] = revision;
        auto changes = Das::Utils::MakeYyjsonArray();
        (*changes.as_array())
            .emplace_back(make_connect_element("e_1", "sink_int", "in_int"));
        (*changes.as_array())
            .emplace_back(make_connect_element("e_2", "sink_str", "in_string"));
        (*change.as_object())[std::string_view("changes")] = std::move(changes);

        auto apply = scheduler_->ApplyTaskAuthoringChange(0, change);
        auto apply_obj = apply.as_object();
        ASSERT_TRUE(apply_obj.has_value());
        EXPECT_TRUE(
            (*apply_obj)[std::string_view("ok")].as_bool().value_or(false))
            << "downstream connects onto the inner dynamic ports were "
               "rejected: "
            << Das::Utils::SerializeYyjsonValue(apply).value_or("");

        auto result_after = scheduler_->GetTaskAuthoringDocument(0, request);
        auto after_obj = result_after.as_object();
        ASSERT_TRUE(after_obj.has_value());
        ASSERT_TRUE(after_obj->contains(std::string_view("document")));
        auto after_doc = (*after_obj)[std::string_view("document")].as_object();
        ASSERT_TRUE(after_doc.has_value());
        ASSERT_TRUE(after_doc->contains(std::string_view("graph")));
        auto graph = (*after_doc)[std::string_view("graph")].as_object();
        ASSERT_TRUE(graph.has_value());
        ASSERT_TRUE(graph->contains(std::string_view("connections")));
        auto connections = (*graph)[std::string_view("connections")].as_array();
        ASSERT_TRUE(connections.has_value());
        const auto edge_present = [&connections](
                                      std::string_view from_port,
                                      std::string_view to_node,
                                      std::string_view to_port)
        {
            for (const auto& connection : *connections)
            {
                auto conn_obj = connection.as_object();
                if (!conn_obj
                    || (*conn_obj)[std::string_view("fromNodeId")]
                               .as_string()
                               .value_or("")
                           != std::string_view("invoke")
                    || (*conn_obj)[std::string_view("fromPortId")]
                               .as_string()
                               .value_or("")
                           != from_port
                    || (*conn_obj)[std::string_view("toNodeId")]
                               .as_string()
                               .value_or("")
                           != to_node
                    || (*conn_obj)[std::string_view("toPortId")]
                               .as_string()
                               .value_or("")
                           != to_port)
                {
                    continue;
                }
                return true;
            }
            return false;
        };
        EXPECT_TRUE(edge_present("e_1", "sink_int", "in_int"))
            << "the int downstream edge did not persist";
        EXPECT_TRUE(edge_present("e_2", "sink_str", "in_string"))
            << "the string downstream edge did not persist";
    }

    // ④ 子图执行编译产物（purpose=execution——与调用节点的条目解析同
    //    通道，creator 会话编译）：boundaryPorts.outputs 携带 e_1/e_2 双口
    //    清单（执行期 typed 边界导出与调用节点逐口投影按这份清单消费），
    //    b_1 进 inputs 清单（父图→子图直塞半）。
    {
        auto compile_request =
            Das::Utils::ParseYyjsonFromString(R"({"purpose":"execution"})");
        ASSERT_TRUE(compile_request.has_value());
        auto result =
            scheduler_->CompileRepositoryEntryAuthoring(7, *compile_request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("compile")))
            << "entry compile failed: "
            << Das::Utils::SerializeYyjsonValue(result).value_or("");
        auto compile_obj = (*obj)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_obj.has_value());
        ASSERT_TRUE(compile_obj->contains(std::string_view("executionInput")));
        auto execution_input =
            (*compile_obj)[std::string_view("executionInput")].as_object();
        ASSERT_TRUE(execution_input.has_value());
        ASSERT_TRUE(execution_input->contains(std::string_view("plan")));
        auto plan = (*execution_input)[std::string_view("plan")].as_object();
        ASSERT_TRUE(plan.has_value());
        ASSERT_TRUE(plan->contains(std::string_view("boundaryPorts")));
        auto boundary_ports =
            (*plan)[std::string_view("boundaryPorts")].as_object();
        ASSERT_TRUE(boundary_ports.has_value());

        // 清单=声明面（end manifest 的「值」口对与镜像口）∪ faces 动态口
        // （b_1/e_1/e_2）——断言动态口在场（containment），不锁清单全长。
        const auto boundary_port_ids_of =
            [](const yyjson::value& ports_value) -> std::set<std::string>
        {
            std::set<std::string> ids;
            auto                  arr = ports_value.as_array();
            if (!arr)
            {
                return ids;
            }
            for (const auto& entry : *arr)
            {
                auto entry_obj = entry.as_object();
                if (!entry_obj)
                {
                    continue;
                }
                if (auto id =
                        (*entry_obj)[std::string_view("portId")].as_string())
                {
                    ids.insert(std::string(*id));
                }
            }
            return ids;
        };

        auto inputs = (*boundary_ports)[std::string_view("inputs")].as_array();
        ASSERT_TRUE(inputs.has_value());
        {
            const auto input_ids = boundary_port_ids_of(*inputs);
            EXPECT_TRUE(input_ids.count("b_1") != 0)
                << "dynamic begin port b_1 missing from the compiled "
                   "boundary inputs";
        }

        auto outputs =
            (*boundary_ports)[std::string_view("outputs")].as_array();
        ASSERT_TRUE(outputs.has_value());
        std::map<std::string, std::string> output_types;
        for (const auto& entry : *outputs)
        {
            auto entry_obj = entry.as_object();
            ASSERT_TRUE(entry_obj.has_value());
            auto id = (*entry_obj)[std::string_view("portId")].as_string();
            ASSERT_TRUE(id.has_value());
            auto type = (*entry_obj)[std::string_view("portType")].as_string();
            ASSERT_TRUE(type.has_value());
            if (std::string_view(*id) == std::string_view("e_1")
                || std::string_view(*id) == std::string_view("e_2"))
            {
                output_types[std::string(*id)] = std::string(*type);
            }
        }
        ASSERT_EQ(output_types.count("e_1"), 1u)
            << "dynamic end port e_1 missing from the compiled boundary "
               "outputs";
        EXPECT_EQ(output_types.at("e_1"), "int");
        ASSERT_EQ(output_types.count("e_2"), 1u)
            << "dynamic end port e_2 missing from the compiled boundary "
               "outputs";
        EXPECT_EQ(output_types.at("e_2"), "string");
    }
}

// 多值边界输出 E2E·执行半（任务级真实执行）：多值图本身作为调度任务——
// begin 1 输入（b_1）+ 常数供值节点（int/string 各一）→ end 2 输出
// （e_1 int / e_2 string）+ 两个下游探针各挂一条数据边（同一对常数值同
// 轮送进 end 动态口与探针）。RunTaskNow 冷启动执行（执行链编译宿主自带
// faces 预备 pass）：两探针各收各值（int/string 分型断言）＝值生产→数据
// 边→分型投递全链实测；end 动态口同值进入边界导出（typed_outputs 逐键
// 直写在 Do 内按编译清单消费）。end「值」口未接线：单 int 返回码路径原
// 样行为（回零，任务正常完成）。
TEST_F(
    SchedulerRuntimeBackedTest,
    MultiValueBoundaryOutputsDeliverTypedValuesAtExecution)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    // 下游收值探针的口面（编译侧边校验需要声明口）。
    InjectPortDeclarationsIntoFactoryManifest(
        manifest_path_,
        FactoryTaskComponentImplGuidString,
        R"json({
            "inputs": [
                {"id": "in_int", "type": "int"},
                {"id": "in_string", "type": "string"}
            ]
        })json");
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);
    DeployRealPluginArtifacts(
        "libDasFlowControl.dll",
        "DasFlowControl.json",
        /*force_ipc=*/false,
        "DasFlowControl");

    {
        auto properties = Das::Utils::ParseYyjsonFromString(R"json({
            "documentId": "multi-value-graph",
            "version": 1,
            "nodes": [
                {
                    "nodeId": "n_begin",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "boundaryPorts": [
                            {"id": "b_1", "type": "string"}
                        ]
                    }
                },
                {
                    "nodeId": "const_int",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "68F1000A-0000-4000-8000-00000000000A",
                            "pluginGuid": "7115BBFA-7E57-494E-82D5-D4134C367E13"
                        }
                    },
                    "settings": {
                        "value": 4242
                    }
                },
                {
                    "nodeId": "const_str",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "68F1000C-0000-4000-8000-00000000000C",
                            "pluginGuid": "7115BBFA-7E57-494E-82D5-D4134C367E13"
                        }
                    },
                    "settings": {
                        "value": "multi-out"
                    }
                },
                {
                    "nodeId": "sink_int",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "97BA1BE0-A199-47CB-9604-440B8BBC6555",
                            "pluginGuid": "12345678-9ABC-4DEF-8123-456789ABCDEF"
                        }
                    },
                    "settings": {}
                },
                {
                    "nodeId": "sink_str",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "97BA1BE0-A199-47CB-9604-440B8BBC6555",
                            "pluginGuid": "12345678-9ABC-4DEF-8123-456789ABCDEF"
                        }
                    },
                    "settings": {}
                },
                {
                    "nodeId": "n_end",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {
                        "boundaryPorts": [
                            {"id": "e_1", "type": "int"},
                            {"id": "e_2", "type": "string"}
                        ]
                    }
                }
            ],
            "edges": [
                {
                    "edgeId": "edge_to_end_int",
                    "sourceNodeId": "const_int",
                    "sourcePortId": "value",
                    "targetNodeId": "n_end",
                    "targetPortId": "e_1",
                    "edgeType": "data"
                },
                {
                    "edgeId": "edge_to_end_str",
                    "sourceNodeId": "const_str",
                    "sourcePortId": "value",
                    "targetNodeId": "n_end",
                    "targetPortId": "e_2",
                    "edgeType": "data"
                },
                {
                    "edgeId": "edge_to_sink_int",
                    "sourceNodeId": "const_int",
                    "sourcePortId": "value",
                    "targetNodeId": "sink_int",
                    "targetPortId": "in_int",
                    "edgeType": "data"
                },
                {
                    "edgeId": "edge_to_sink_str",
                    "sourceNodeId": "const_str",
                    "sourcePortId": "value",
                    "targetNodeId": "sink_str",
                    "targetPortId": "in_string",
                    "edgeType": "data"
                }
            ],
            "tags": []
        })json");
        ASSERT_TRUE(properties.has_value());
        yyjson::value task0(Das::Utils::MakeYyjsonObject());
        (*task0.as_object())[std::string_view("id")] = 0;
        (*task0.as_object())[std::string_view("taskGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        (*task0.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*task0.as_object())[std::string_view("nextExecutionTime")] =
            static_cast<int64_t>(1);
        {
            yyjson::value authoring(Das::Utils::MakeYyjsonObject());
            (*authoring.as_object())[std::string_view("revision")] = 1;
            (*task0.as_object())[std::string_view("authoring")] =
                std::move(authoring);
        }
        (*task0.as_object())[std::string_view("properties")] =
            std::move(*properties);
        WriteSchedulerState(*settings_manager_, 1, {0}, {task0});
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());

    // 任务级 Get：图文档根 ports section 三口全量（b_1 入 / e_1、e_2 出）
    // ——预备 pass 注入 begin/end faces 后的推导边界投影。
    {
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")))
            << "task Get failed: "
            << Das::Utils::SerializeYyjsonValue(result).value_or("");
        auto doc_obj = (*obj)[std::string_view("document")].as_object();
        ASSERT_TRUE(doc_obj.has_value());
        ASSERT_TRUE(doc_obj->contains(std::string_view("ports")))
            << "graph task document carries no root ports section";
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports.has_value());
        ASSERT_EQ(ports->size(), 3u);
        const auto port_of = [&ports](std::size_t index)
        { return (*ports)[index].as_object(); };
        ASSERT_TRUE(port_of(0).has_value());
        EXPECT_EQ(
            (*port_of(0))[std::string_view("id")].as_string().value_or(""),
            "b_1");
        EXPECT_EQ(
            (*port_of(0))[std::string_view("direction")].as_string().value_or(
                ""),
            "input");
        ASSERT_TRUE(port_of(1).has_value());
        EXPECT_EQ(
            (*port_of(1))[std::string_view("id")].as_string().value_or(""),
            "e_1");
        EXPECT_EQ(
            (*port_of(1))[std::string_view("direction")].as_string().value_or(
                ""),
            "output");
        ASSERT_TRUE(port_of(2).has_value());
        EXPECT_EQ(
            (*port_of(2))[std::string_view("id")].as_string().value_or(""),
            "e_2");
        EXPECT_EQ(
            (*port_of(2))[std::string_view("type")].as_string().value_or(""),
            "string");
        EXPECT_EQ(
            (*port_of(2))[std::string_view("direction")].as_string().value_or(
                ""),
            "output");
    }

    // 冷启动执行（无 UI 预热）：两探针各收各值。
    ASSERT_EQ(scheduler_->RunTaskNow(0), DAS_S_OK);
    {
        // 观察窗按全量 -j 并行重标定：图执行的每跳是跨 host 往返，
        // 32 路并发争抢下 30 秒窗会被挤爆（隔离态数秒即达）；健康轮
        // 秒级收敛，放大上限不拖慢。
        // 等待谓词必须是断言主语本身：component_do_count 在 Do 入口即
        // 递增、先于收值观测落账，拿计数当完成信号会在最后一个 Do 的
        // 计数与落账之间竞态误判缺值（隔离态 ~40% 复现）。
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(90);
        bool inputs_recorded = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                const ipc_scoped_lock lock(shared_state_->mutex);
                inputs_recorded =
                    shared_state_->received_input_int_present
                    && shared_state_->received_input_string_present;
            }
            if (inputs_recorded)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(inputs_recorded)
            << "typed port entries not recorded by sink nodes"
            << " (component_do_count=" << shared_state_->component_do_count
            << ")";

        const ipc_scoped_lock lock(shared_state_->mutex);
        ASSERT_TRUE(shared_state_->received_input_int_present)
            << "no int-typed port entry reached any sink node";
        EXPECT_EQ(
            std::string_view(shared_state_->received_input_int_port),
            std::string_view("in_int"));
        EXPECT_EQ(shared_state_->received_input_int_value, 4242);
        ASSERT_TRUE(shared_state_->received_input_string_present)
            << "no string-typed port entry reached any sink node";
        EXPECT_EQ(
            std::string_view(shared_state_->received_input_string_port),
            std::string_view("in_string"));
        EXPECT_EQ(
            std::string_view(shared_state_->received_input_string_value),
            std::string_view("multi-out"));
    }

    // 收尾：等 BusinessThread 尾段（GetNextExecutionTime/持久化）落地，
    // 避免 TearDown 与执行尾段竞态（照既有执行系用例）。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// 预备 pass·跳过分支：无注册面记录的 componentRef 节点（纯 manifest 或未
// 知组件）不注入任何 face——Get 正常、无动态口。非图任务（线性文档）零
// faces 键：Fake 会话收到的 context 与现状一致（无 nodeFaces/entryFaces
// 消费面，既有契约用例背书）。
TEST_F(SchedulerRuntimeBackedTest, GraphFacesPassSkipsNodesWithoutCapability)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    {
        auto properties = Das::Utils::ParseYyjsonFromString(R"json({
            "documentId": "faces-skip-graph",
            "version": 1,
            "nodes": [
                {
                    "nodeId": "n_begin",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {}
                },
                {
                    "nodeId": "unknown_component",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "11111111-2222-4333-8444-555555555555",
                            "pluginGuid": "12345678-9ABC-4DEF-8123-456789ABCDEF"
                        }
                    },
                    "settings": {}
                }
            ],
            "edges": [],
            "tags": []
        })json");
        ASSERT_TRUE(properties.has_value());
        SeedGraphTaskInstance(*settings_manager_, std::move(*properties));
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto result = scheduler_->GetTaskAuthoringDocument(0, request);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE(obj->contains(std::string_view("document")));
    auto node = FindGraphNodeByComponent(
        (*obj)[std::string_view("document")],
        std::string_view("11111111-2222-4333-8444-555555555555"));
    ASSERT_TRUE(node.has_value());
    EXPECT_FALSE(NodeProjectsPort(*node, "p1"))
        << "face injected for a node whose component has no registry record";
}

// 执行链冷启动 faces 同源：无任何 UI Get 预热，RunTaskNow 直接执行图任务
// ——ExecuteSelectedTask 建编译会话前跑同一 faces 预备 pass（与 HTTP 管线
// 外圈同款调用）：未绑 componentRef 节点（组件有 authoring 会话）经瞬态
// 会话 GetDocument 现算口面注入执行编译 context。观察通道=Fake 会话计数
// 器：get_document_count 只有 GetDocument 递增，而执行链在修复前不触任
// 何组件文档 Get（图任务编译走真实 DasGraphTask 会话，Fake 计数恒零）。
TEST_F(SchedulerRuntimeBackedTest, ExecutionChainColdStartRunsFacesPrepPass)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    {
        auto properties = Das::Utils::ParseYyjsonFromString(R"json({
            "documentId": "faces-coldstart-graph",
            "version": 1,
            "nodes": [
                {
                    "nodeId": "n_begin",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
                            "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                        }
                    },
                    "settings": {}
                },
                {
                    "nodeId": "probe",
                    "target": {
                        "targetKind": "componentRef",
                        "componentRef": {
                            "kind": "componentRef",
                            "componentGuid": "97BA1BE0-A199-47CB-9604-440B8BBC6555",
                            "pluginGuid": "12345678-9ABC-4DEF-8123-456789ABCDEF"
                        }
                    },
                    "settings": {}
                }
            ],
            "edges": [],
            "tags": []
        })json");
        ASSERT_TRUE(properties.has_value());
        SeedGraphTaskInstance(*settings_manager_, std::move(*properties));
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    ASSERT_EQ(scheduler_->Status(), SchedulerState::Stopped);

    // Fake 会话走严格 envelope 形态并开 ports section（faces 注入链全形
    // 态走通——口面快照真实产出，非仅计数）。
    EnableStrictAuthoringWire(*shared_state_);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->emit_ports_section = true;
        shared_state_->set_emit_ports_section_id("e1");
    }

    size_t get_document_before = 0;
    size_t session_before = 0;
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        get_document_before = shared_state_->get_document_count;
        session_before = shared_state_->authoring_session_count;
    }

    // 冷启动执行：此前零 Get/Apply/Compile 调用（无 UI 预热）。
    ASSERT_EQ(scheduler_->RunTaskNow(0), DAS_S_OK);

    // 等待执行链编译消费组件文档口面（预备 pass 的瞬态会话 GetDocument）。
    bool faces_consulted = false;
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                const ipc_scoped_lock lock(shared_state_->mutex);
                faces_consulted =
                    shared_state_->get_document_count > get_document_before;
            }
            if (faces_consulted)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(faces_consulted)
        << "执行链编译未消费组件文档口面（faces 预备 pass 未在执行链跑）";
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_GT(shared_state_->authoring_session_count, session_before)
            << "预备 pass 未为 componentRef 节点建瞬态 authoring 会话";
    }

    // 收尾：等图任务 Do 完成（probe 组件执行），避免 TearDown 与执行尾段
    // 竞态。
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool probe_done = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                const ipc_scoped_lock lock(shared_state_->mutex);
                probe_done = shared_state_->component_do_count >= 1;
            }
            if (probe_done)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_TRUE(probe_done) << "graph task probe component never executed";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// 组件语境标记透传链：authoring get/apply/compile 请求体的可选布尔键
// componentContext（缺省 false）→ SchedulerService context 装配 →
// CreateSession 收到的 context_json（MaaPI 工厂消费在后续 plan）。带 true
// → context 含该键 true；不带 → context 无该键（工厂 contains 探测不
// 到）；畸形类型（字符串 "true"）按缺省处理不炸路由。观察通道=Fake 工厂
// 的 last_context_* 记录（会话创建时快照）；阶段间用 apply 推进任务
// revision 强制会话重建以观察新 context。
TEST_F(
    SchedulerRuntimeBackedTest,
    AuthoringComponentContextFlagFlowsIntoContext)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    SeedEmptyAuthoringTask(*settings_manager_, 0, 0);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    EnableStrictAuthoringWire(*shared_state_);

    // ① 缺省（无键）：context 无该键。
    {
        const yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")));
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_FALSE(shared_state_->last_context_had_component_context);
        EXPECT_FALSE(shared_state_->last_context_component_context);
    }

    // ② 带 true：apply 推进 revision（重建会话）后 Get，context 含该键
    //    true。
    {
        auto apply = scheduler_->ApplyTaskAuthoringChange(
            0,
            MakeStrictAuthoringChange(0));
        auto apply_obj = apply.as_object();
        ASSERT_TRUE(apply_obj.has_value());
        EXPECT_TRUE(
            (*apply_obj)[std::string_view("ok")].as_bool().value_or(false));

        yyjson::value request(Das::Utils::MakeYyjsonObject());
        (*request.as_object())[std::string_view("componentContext")] = true;
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")));
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_TRUE(shared_state_->last_context_had_component_context);
        EXPECT_TRUE(shared_state_->last_context_component_context);
    }

    // ③ 畸形类型（字符串 "true"）：按缺省处理——路由不炸，context 不含
    //    true 值。
    {
        auto apply = scheduler_->ApplyTaskAuthoringChange(
            0,
            MakeStrictAuthoringChange(1));
        auto apply_obj = apply.as_object();
        ASSERT_TRUE(apply_obj.has_value());
        EXPECT_TRUE(
            (*apply_obj)[std::string_view("ok")].as_bool().value_or(false));

        yyjson::value request(Das::Utils::MakeYyjsonObject());
        (*request.as_object())[std::string_view("componentContext")] =
            std::make_pair(std::string_view("true"), yyjson::copy_string);
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        ASSERT_TRUE(obj->contains(std::string_view("document")));
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_FALSE(shared_state_->last_context_component_context)
            << "malformed (string) componentContext must fall back to the "
               "absent default";
    }
}

// graph authoring 的 catalog 端口生产端全链：manifest definition 的独立
// signals[] 声明（DasMaaPi 形状——条目只有 {id}）、端口级多语言 label map
// 解析（默认 locale=en 取 map 的 en 值；miss/畸形回退 portId，显示数据
// 畸形不丢端口）、空 type 条目跳过（单条畸形不再进入通道），经真实调度器
// → context → DasGraphTask 会话 → Core 投影落到 GetDocument 的节点
// ports[]（五键）。
TEST_F(
    SchedulerRuntimeBackedTest,
    GraphAuthoringCatalogProjectsSignalsAndPortLabels)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    InjectPortDeclarationsIntoFactoryManifest(
        manifest_path_,
        FactoryTaskComponentImplGuidString,
        R"json({
            "inputs": [
                {"id": "in", "type": "object"},
                {"id": "bad_empty_type", "type": ""},
                {"id": "labeled_in", "type": "bool",
                 "label": {"en": "Labeled In", "zh-cn": "已标注输入"}},
                {"id": "labeled_en_only", "type": "bool",
                 "label": {"en": "En Only"}},
                {"id": "labeled_empty_value", "type": "bool",
                 "label": {"en": ""}},
                {"id": "bad_label_type", "type": "bool", "label": 123}
            ],
            "outputs": [
                {"id": "out", "type": "array<string>"}
            ],
            "signals": [
                {"id": "succeeded"},
                {"id": "failed"},
                {"id": "cancelled"}
            ]
        })json");
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    // graph 任务实例：properties 携带一个 componentRef 节点指向探针组件，
    // 其静态端口面（inputs/outputs/signals）应投影到该节点的 ports[]。
    {
        yyjson::value task0(Das::Utils::MakeYyjsonObject());
        (*task0.as_object())[std::string_view("id")] = 0;
        (*task0.as_object())[std::string_view("taskGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        (*task0.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*task0.as_object())[std::string_view("nextExecutionTime")] =
            static_cast<int64_t>(1);
        yyjson::value properties(Das::Utils::MakeYyjsonObject());
        auto          props_obj = properties.as_object();
        (*props_obj)[std::string_view("documentId")] = "port-probe-graph";
        (*props_obj)[std::string_view("version")] = 1;
        yyjson::value nodes(Das::Utils::MakeYyjsonArray());
        {
            yyjson::value node(Das::Utils::MakeYyjsonObject());
            (*node.as_object())[std::string_view("nodeId")] = "probe";
            yyjson::value target(Das::Utils::MakeYyjsonObject());
            (*target.as_object())[std::string_view("targetKind")] =
                "componentRef";
            yyjson::value component_ref(Das::Utils::MakeYyjsonObject());
            (*component_ref.as_object())[std::string_view("kind")] =
                "componentRef";
            (*component_ref.as_object())[std::string_view("componentGuid")] =
                std::string(FactoryTaskComponentImplGuidString);
            (*component_ref.as_object())[std::string_view("pluginGuid")] =
                std::string(FactoryPluginGuidString);
            (*target.as_object())[std::string_view("componentRef")] =
                std::move(component_ref);
            (*node.as_object())[std::string_view("target")] = std::move(target);
            (*node.as_object())[std::string_view("settings")] =
                yyjson::value(Das::Utils::MakeYyjsonObject());
            nodes.as_array()->emplace_back(std::move(node));
        }
        (*props_obj)[std::string_view("nodes")] = std::move(nodes);
        (*props_obj)[std::string_view("edges")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*props_obj)[std::string_view("tags")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*task0.as_object())[std::string_view("properties")] =
            std::move(properties);
        WriteSchedulerState(*settings_manager_, 1, {0}, {task0});
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto result = scheduler_->GetTaskAuthoringDocument(0, request);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE(obj->contains(std::string_view("document")));
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    ASSERT_TRUE(document->contains(std::string_view("graph")));
    auto graph = (*document)[std::string_view("graph")].as_object();
    ASSERT_TRUE(graph.has_value());
    ASSERT_TRUE(graph->contains(std::string_view("nodes")));
    auto nodes = (*graph)[std::string_view("nodes")].as_array();
    ASSERT_TRUE(nodes.has_value());
    ASSERT_EQ(nodes->size(), 1u);
    auto node_obj = (*nodes)[0].as_object();
    ASSERT_TRUE(node_obj.has_value());
    ASSERT_TRUE(node_obj->contains(std::string_view("ports")));
    auto ports = (*node_obj)[std::string_view("ports")].as_array();
    ASSERT_TRUE(ports.has_value());

    // 端口查找替身：按 portId 定位并返回端口对象的克隆。
    const auto port_of =
        [&ports](std::string_view port_id) -> std::optional<yyjson::value>
    {
        for (const auto& port : *ports)
        {
            auto port_object = port.as_object();
            if (!port_object)
            {
                continue;
            }
            auto id = (*port_object)[std::string_view("portId")].as_string();
            if (id && *id == port_id)
            {
                return Das::Utils::CloneYyjsonValue(port);
            }
        }
        return std::nullopt;
    };
    const auto string_field =
        [](const yyjson::value& port_value, std::string_view key)
    {
        auto port_object = port_value.as_object();
        if (!port_object)
        {
            return std::string{};
        }
        return std::string((*port_object)[key].as_string().value_or(""));
    };

    // 期望端口总数：inputs 5（空 type 条目被跳过；label 畸形不丢端口）+
    // outputs 1 + signals 3。
    ASSERT_EQ(ports->size(), 9u);

    // inputs：方向 input；label map 解析用之，缺失回退 portId。
    const auto in_port = port_of("in");
    ASSERT_TRUE(in_port.has_value());
    EXPECT_EQ(string_field(*in_port, "direction"), "input");
    EXPECT_EQ(string_field(*in_port, "portType"), "object");
    EXPECT_EQ(string_field(*in_port, "displayLabel"), "in");
    EXPECT_EQ(string_field(*in_port, "category"), "data");
    const auto labeled_port = port_of("labeled_in");
    ASSERT_TRUE(labeled_port.has_value());
    EXPECT_EQ(string_field(*labeled_port, "displayLabel"), "Labeled In");
    EXPECT_EQ(string_field(*labeled_port, "category"), "data");

    // map 仅含 en：默认 locale=en 命中 en 值。
    const auto en_only_port = port_of("labeled_en_only");
    ASSERT_TRUE(en_only_port.has_value());
    EXPECT_EQ(string_field(*en_only_port, "displayLabel"), "En Only");
    EXPECT_EQ(string_field(*en_only_port, "direction"), "input");

    // 空 map 值视为 miss：回退 portId。
    const auto empty_value_port = port_of("labeled_empty_value");
    ASSERT_TRUE(empty_value_port.has_value());
    EXPECT_EQ(
        string_field(*empty_value_port, "displayLabel"),
        "labeled_empty_value");

    // label 非 object 亦非 string（数字字面量）：端口仍投影，displayLabel
    // 回退 portId——显示数据畸形不丢端口。
    const auto bad_label_port = port_of("bad_label_type");
    ASSERT_TRUE(bad_label_port.has_value());
    EXPECT_EQ(string_field(*bad_label_port, "displayLabel"), "bad_label_type");

    // 空 type 的畸形条目不进入通道（不再产出空 portType 条目）。
    EXPECT_FALSE(port_of("bad_empty_type").has_value());

    // outputs：方向 output，displayLabel 回退 portId。
    const auto out_port = port_of("out");
    ASSERT_TRUE(out_port.has_value());
    EXPECT_EQ(string_field(*out_port, "direction"), "output");
    EXPECT_EQ(string_field(*out_port, "portType"), "array<string>");
    EXPECT_EQ(string_field(*out_port, "displayLabel"), "out");
    EXPECT_EQ(string_field(*out_port, "category"), "data");

    // signals[]（DasMaaPi 形状：条目只有 {id}）：方向 output、portType 投影
    // 为 signal、displayLabel 回退 portId、类别 execution。
    for (const std::string_view signal_id :
         {"succeeded", "failed", "cancelled"})
    {
        const auto signal_port = port_of(signal_id);
        ASSERT_TRUE(signal_port.has_value()) << signal_id;
        EXPECT_EQ(string_field(*signal_port, "direction"), "output")
            << signal_id;
        EXPECT_EQ(string_field(*signal_port, "portType"), "signal")
            << signal_id;
        EXPECT_EQ(string_field(*signal_port, "displayLabel"), signal_id);
        EXPECT_EQ(string_field(*signal_port, "category"), "execution")
            << signal_id;
    }
}

namespace
{
    // begin/end 边界文档管线用例共用：播种一个 graph 仓库条目（宿主图），
    // acceptedProperties 为 raw GraphDocumentDto 形状（begin 节点带
    // boundaryPorts，other 探针节点，可选边集）。返回播种后的条目 json。
    // document_version：宿主图文档 version 字段（生产不变量=与
    // authoring.revision 同步推进；宿主自身 Apply 的 revision 域校验依赖
    // 该同步。边界管线用例缺省 1 即可——它们只走边界会话通道）。
    yyjson::value SeedGraphRepositoryEntry(
        Das::Core::SettingsManager::SettingsManager& settings,
        int64_t                                      entry_id,
        std::string_view                             edges_json,
        int64_t                                      document_version = 1)
    {
        yyjson::value entry(Das::Utils::MakeYyjsonObject());
        (*entry.as_object())[std::string_view("entryId")] = entry_id;
        (*entry.as_object())[std::string_view("displayName")] =
            "Boundary host graph";
        (*entry.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*entry.as_object())[std::string_view("taskTypeGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        {
            yyjson::value authoring(Das::Utils::MakeYyjsonObject());
            (*authoring.as_object())[std::string_view("revision")] = 5;
            (*authoring.as_object())[std::string_view("lastChangeKind")] =
                std::make_pair(
                    std::string_view("addNodePort"),
                    yyjson::copy_string);
            (*authoring.as_object())[std::string_view("sourceFingerprint")] =
                std::make_pair(
                    std::string_view("boundary-seed"),
                    yyjson::copy_string);
            (*entry.as_object())[std::string_view("authoring")] =
                std::move(authoring);
        }
        {
            auto properties = Das::Utils::ParseYyjsonFromString(
                std::string(
                    R"json({
                    "documentId": "boundary-host-graph",
                    "version": )json")
                + std::to_string(document_version) + R"json(,
                    "nodes": [
                        {
                            "nodeId": "n_begin",
                            "target": {
                                "targetKind": "componentRef",
                                "componentRef": {
                                    "kind": "componentRef",
                                    "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E",
                                    "pluginGuid": "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D"
                                }
                            },
                            "settings": {
                                "boundaryPorts": [
                                    {"id": "p1", "type": "string"}
                                ]
                            }
                        },
                        {
                            "nodeId": "other",
                            "target": {
                                "targetKind": "componentRef",
                                "componentRef": {
                                    "kind": "componentRef",
                                    "componentGuid": "97BA1BE0-A199-47CB-9604-440B8BBC6555",
                                    "pluginGuid": "12345678-9ABC-4DEF-8123-456789ABCDEF"
                                }
                            },
                            "settings": {}
                        }
                    ],
                    "edges": )json"
                + std::string(edges_json) + R"json(,
                    "tags": []
                })json");
            EXPECT_TRUE(properties.has_value());
            (*entry.as_object())[std::string_view("acceptedProperties")] =
                std::move(*properties);
        }
        (*entry.as_object())[std::string_view("graphDocument")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        {
            yyjson::value availability(Das::Utils::MakeYyjsonObject());
            (*availability.as_object())[std::string_view("state")] =
                "available";
            (*entry.as_object())[std::string_view("availability")] =
                std::move(availability);
        }
        EXPECT_EQ(
            settings.UpdateTaskRepositoryEntryJson("0", entry_id, entry),
            DAS_S_OK);
        return entry;
    }

    // 宿主图文档里指定节点的 settings 克隆（存储断言用）。
    std::optional<yyjson::value> HostGraphNodeSettings(
        const yyjson::value& properties,
        std::string_view     node_id)
    {
        auto props_obj = properties.as_object();
        if (!props_obj || !props_obj->contains(std::string_view("nodes")))
        {
            return std::nullopt;
        }
        const auto& nodes_ref = (*props_obj)[std::string_view("nodes")];
        auto        nodes_arr = nodes_ref.as_array();
        if (!nodes_arr)
        {
            return std::nullopt;
        }
        for (const auto& node_value : *nodes_arr)
        {
            auto node_obj = node_value.as_object();
            if (!node_obj || !node_obj->contains(std::string_view("nodeId")))
            {
                continue;
            }
            auto current_id =
                (*node_obj)[std::string_view("nodeId")].as_string();
            if (current_id && *current_id == node_id)
            {
                if (node_obj->contains(std::string_view("settings")))
                {
                    const auto& settings_ref =
                        (*node_obj)[std::string_view("settings")];
                    return Das::Utils::CloneYyjsonValue(settings_ref);
                }
                return yyjson::value(Das::Utils::MakeYyjsonObject());
            }
        }
        return std::nullopt;
    }

    std::optional<yyjson::value> EntryAcceptedProperties(
        Das::Core::SettingsManager::SettingsManager& settings,
        int64_t                                      entry_id)
    {
        auto entry_json = settings.GetTaskRepositoryEntryJson("0", entry_id);
        auto entry_obj = entry_json.as_object();
        if (!entry_obj
            || !entry_obj->contains(std::string_view("acceptedProperties")))
        {
            return std::nullopt;
        }
        const auto& props_ref =
            (*entry_obj)[std::string_view("acceptedProperties")];
        return Das::Utils::CloneYyjsonValue(props_ref);
    }
} // namespace

// begin/end 节点文档 Get（MaaPI 对齐管线侧）：请求体 componentGuid 键承载
// begin 身份（宿主图身份走既有条目 URL 通道；无 nodeId 请求体键）——经既有
// capability QI 链取回边界文档（items 写通道+ports 读通道+chrome 三键，
// direction 角色固化 input）。纯 Get 零写回：条目存储逐字节不变。
TEST_F(
    SchedulerRuntimeBackedTest,
    BoundaryDocumentGetViaBeginGuidOnRepositoryEntry)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    SeedGraphRepositoryEntry(*settings_manager_, 3, "[]");
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const auto entry_json_before =
        settings_manager_->GetTaskRepositoryEntryJson("0", 3);

    yyjson::value request(Das::Utils::MakeYyjsonObject());
    (*request.as_object())[std::string_view("componentGuid")] = std::make_pair(
        Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID,
        yyjson::copy_string);
    (*request.as_object())[std::string_view("componentContext")] = true;

    auto result = scheduler_->GetRepositoryEntryAuthoringDocument(3, request);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_FALSE(obj->contains(std::string_view("errorKind")));
    ASSERT_TRUE(obj->contains(std::string_view("document")));
    {
        const auto doc_text = Das::Utils::SerializeYyjsonValue(
            (*obj)[std::string_view("document")],
            true);
        const auto accepted_text = Das::Utils::SerializeYyjsonValue(
            (*obj)[std::string_view("acceptedProperties")],
            true);
        const std::string doc_str = doc_text.value_or("");
        const std::string accp_str = accepted_text.value_or("");
        printf(
            "TEMP-DIAG boundary doc=%.*s\nTEMP-DIAG accepted=%.*s\n",
            static_cast<int>(doc_str.size()),
            doc_str.data(),
            static_cast<int>(accp_str.size()),
            accp_str.data());
    }

    EXPECT_EQ(
        (*obj)[std::string_view("taskTypeGuid")].as_string().value_or(""),
        Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID);
    EXPECT_EQ((*obj)[std::string_view("revision")].as_sint().value_or(-1), 5);

    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(
        (*document)[std::string_view("viewKind")].as_string().value_or(""),
        "formSequence");
    // items 写通道：boundaryPorts 一一对应。
    auto form_sequence =
        (*document)[std::string_view("formSequence")].as_object();
    ASSERT_TRUE(form_sequence.has_value());
    auto items = (*form_sequence)[std::string_view("items")].as_array();
    ASSERT_TRUE(items.has_value());
    EXPECT_EQ(items->size(), 1u);
    // ports 读通道：方向角色固化（begin=input）。
    auto ports = (*document)[std::string_view("ports")].as_array();
    ASSERT_TRUE(ports.has_value());
    ASSERT_EQ(ports->size(), 1u);
    {
        auto port_obj = (*ports)[0].as_object();
        ASSERT_TRUE(port_obj.has_value());
        EXPECT_EQ(
            (*port_obj)[std::string_view("id")].as_string().value_or(""),
            "p1");
        EXPECT_EQ(
            (*port_obj)[std::string_view("type")].as_string().value_or(""),
            "string");
        EXPECT_EQ(
            (*port_obj)[std::string_view("direction")].as_string().value_or(""),
            "input");
    }
    // chrome 三键（engaged 文案）。
    for (const std::string_view chrome_key :
         {"addButtonLabel", "emptyStateHint", "listTitle"})
    {
        EXPECT_TRUE(form_sequence->contains(chrome_key))
            << "chrome key missing: " << chrome_key;
    }
    // acceptedProperties 随行（节点 settings 原文）。
    {
        auto accepted =
            (*obj)[std::string_view("acceptedProperties")].as_object();
        ASSERT_TRUE(accepted.has_value());
        EXPECT_FALSE(accepted->contains(std::string_view("componentContext")))
            << "transient request flag leaked into node settings";
        auto boundary_ports =
            (*accepted)[std::string_view("boundaryPorts")].as_array();
        ASSERT_TRUE(boundary_ports.has_value());
        EXPECT_EQ(boundary_ports->size(), 1u);
    }

    // 纯 Get 零写回：条目存储逐字节不变。
    const auto entry_json_after =
        settings_manager_->GetTaskRepositoryEntryJson("0", 3);
    EXPECT_EQ(
        Das::Utils::SerializeYyjsonValue(entry_json_before, true).value_or(""),
        Das::Utils::SerializeYyjsonValue(entry_json_after, true).value_or(""));
}

// begin/end 节点文档 Apply 增口（MaaPI 对齐管线侧）：addFormSequenceItem
// （payload {actionId}=begin guid——与 MaaPI 同款通道形态，URL 载宿主身份、
// 请求体不载节点身份）→ 会话解释为口操作 → 产物=节点 settings 新值写回
// 宿主图文档节点 → 随条目既有 persist 落盘 + revision 推进（零新写回通
// 道）。componentContext 请求期标记不落存储。
TEST_F(
    SchedulerRuntimeBackedTest,
    BoundaryDocumentApplyAddItemOnRepositoryEntry)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    SeedGraphRepositoryEntry(*settings_manager_, 3, "[]");
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    yyjson::value change(Das::Utils::MakeYyjsonObject());
    (*change.as_object())[std::string_view("baseRevision")] =
        static_cast<int64_t>(5);
    {
        yyjson::value element(Das::Utils::MakeYyjsonObject());
        (*element.as_object())[std::string_view("kind")] = std::make_pair(
            std::string_view("addFormSequenceItem"),
            yyjson::copy_string);
        yyjson::value payload(Das::Utils::MakeYyjsonObject());
        (*payload.as_object())[std::string_view("actionId")] = std::make_pair(
            Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID,
            yyjson::copy_string);
        (*element.as_object())[std::string_view("payload")] =
            std::move(payload);
        auto changes = Das::Utils::MakeYyjsonArray();
        (*changes.as_array()).emplace_back(std::move(element));
        (*change.as_object())[std::string_view("changes")] = std::move(changes);
    }
    (*change.as_object())[std::string_view("componentGuid")] = std::make_pair(
        Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID,
        yyjson::copy_string);
    (*change.as_object())[std::string_view("componentContext")] = true;

    auto result = scheduler_->ApplyRepositoryEntryAuthoringChange(3, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_FALSE(obj->contains(std::string_view("errorKind")));
    EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
    EXPECT_EQ((*obj)[std::string_view("revision")].as_sint().value_or(-1), 6);

    // 存储对账：begin 节点 settings.boundaryPorts 增至 2（p1 + 分配的
    // p_2）；无 componentContext 泄漏；图结构其余部分保留（other 节点与
    // 顶层键）。
    auto properties = EntryAcceptedProperties(*settings_manager_, 3);
    ASSERT_TRUE(properties.has_value());
    auto begin_settings = HostGraphNodeSettings(*properties, "n_begin");
    ASSERT_TRUE(begin_settings.has_value());
    {
        auto begin_obj = begin_settings->as_object();
        ASSERT_TRUE(begin_obj.has_value());
        EXPECT_FALSE(begin_obj->contains(std::string_view("componentContext")))
            << "transient request flag leaked into persisted node settings";
        auto ports_arr =
            (*begin_obj)[std::string_view("boundaryPorts")].as_array();
        ASSERT_TRUE(ports_arr.has_value());
        EXPECT_EQ(ports_arr->size(), 2u);
    }
    auto other_settings = HostGraphNodeSettings(*properties, "other");
    ASSERT_TRUE(other_settings.has_value());
    // 宿主 revision 推进（条目 authoring.revision=6）。
    {
        auto entry_json = settings_manager_->GetTaskRepositoryEntryJson("0", 3);
        auto entry_obj = entry_json.as_object();
        ASSERT_TRUE(entry_obj.has_value());
        auto authoring =
            (*entry_obj)[std::string_view("authoring")].as_object();
        ASSERT_TRUE(authoring.has_value());
        EXPECT_EQ(
            (*authoring)[std::string_view("revision")].as_sint().value_or(-1),
            6);
    }
}

// 边界写回后宿主缓存会话新鲜度时序：Get 建会话（D0, rev5）→ boundary
// apply 落盘 D1/rev6（begin 增口 p_2）→ 宿主自身 Apply（缓存命中分支，
// enforce_fresh=false）必须编译/突变自 D1——写回通道落盘后逐出宿主缓存
// 会话，否则缓存会话带着 D0 续命，后续宿主 apply 的持久化会把边界编辑
// 覆盖丢失（begin 口回退 p1、p_2 消失）。
TEST_F(SchedulerRuntimeBackedTest, BoundaryWriteBackEvictsHostGraphSessionCache)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    // document_version=5 与 authoring.revision 同步（宿主自身 Apply 的
    // revision 域校验依赖该生产不变量）。
    SeedGraphRepositoryEntry(
        *settings_manager_,
        3,
        "[]",
        /*document_version=*/5);
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // ① UI Get 建宿主缓存会话（D0 视图：begin 口 p1，rev5）。
    {
        const yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto                result =
            scheduler_->GetRepositoryEntryAuthoringDocument(3, request);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        EXPECT_FALSE(obj->contains(std::string_view("errorKind")));
        ASSERT_TRUE(obj->contains(std::string_view("document")));
    }

    // ② boundary apply 增口（写回 D1/rev6：begin boundaryPorts p1+p_2）。
    {
        yyjson::value change(Das::Utils::MakeYyjsonObject());
        (*change.as_object())[std::string_view("baseRevision")] =
            static_cast<int64_t>(5);
        {
            yyjson::value element(Das::Utils::MakeYyjsonObject());
            (*element.as_object())[std::string_view("kind")] = std::make_pair(
                std::string_view("addFormSequenceItem"),
                yyjson::copy_string);
            yyjson::value payload(Das::Utils::MakeYyjsonObject());
            (*payload.as_object())[std::string_view("actionId")] =
                std::make_pair(
                    Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID,
                    yyjson::copy_string);
            (*element.as_object())[std::string_view("payload")] =
                std::move(payload);
            auto changes = Das::Utils::MakeYyjsonArray();
            (*changes.as_array()).emplace_back(std::move(element));
            (*change.as_object())[std::string_view("changes")] =
                std::move(changes);
        }
        (*change.as_object())[std::string_view("componentGuid")] =
            std::make_pair(
                Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID,
                yyjson::copy_string);
        (*change.as_object())[std::string_view("componentContext")] = true;

        auto result =
            scheduler_->ApplyRepositoryEntryAuthoringChange(3, change);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        EXPECT_FALSE(obj->contains(std::string_view("errorKind")));
        EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
        EXPECT_EQ(
            (*obj)[std::string_view("revision")].as_sint().value_or(-1),
            6);
    }

    // ③ 宿主自身 Apply（baseRevision=6 过 revision 墙；缓存若未逐出则命中
    // D0 旧会话）：加一个普通节点。成功后落盘文档必须同时保有边界编辑
    // （begin 口 p1+p_2）与新节点——旧会话续命则会用 D0 覆盖（p_2 丢）。
    {
        yyjson::value change(Das::Utils::MakeYyjsonObject());
        (*change.as_object())[std::string_view("baseRevision")] =
            static_cast<int64_t>(6);
        {
            yyjson::value element(Das::Utils::MakeYyjsonObject());
            (*element.as_object())[std::string_view("kind")] = std::make_pair(
                std::string_view("addGraphNode"),
                yyjson::copy_string);
            yyjson::value payload(Das::Utils::MakeYyjsonObject());
            (*payload.as_object())[std::string_view("actionId")] =
                std::make_pair(
                    Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID,
                    yyjson::copy_string);
            (*element.as_object())[std::string_view("payload")] =
                std::move(payload);
            auto changes = Das::Utils::MakeYyjsonArray();
            (*changes.as_array()).emplace_back(std::move(element));
            (*change.as_object())[std::string_view("changes")] =
                std::move(changes);
        }

        auto result =
            scheduler_->ApplyRepositoryEntryAuthoringChange(3, change);
        auto obj = result.as_object();
        ASSERT_TRUE(obj.has_value());
        EXPECT_FALSE(obj->contains(std::string_view("errorKind")))
            << "host apply after boundary write-back failed: "
            << Das::Utils::SerializeYyjsonValue(result).value_or("");
        EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
    }

    // ④ 存储对账：新节点在（3 节点）+ 边界编辑保住（begin 口 2 个）+
    // 宿主 revision 再推进（7）。
    {
        auto properties = EntryAcceptedProperties(*settings_manager_, 3);
        ASSERT_TRUE(properties.has_value());
        auto props_obj = properties->as_object();
        ASSERT_TRUE(props_obj.has_value());
        ASSERT_TRUE(props_obj->contains(std::string_view("nodes")));
        auto nodes = (*props_obj)[std::string_view("nodes")].as_array();
        ASSERT_TRUE(nodes.has_value());
        EXPECT_EQ(nodes->size(), 3u)
            << "host apply node add lost after boundary write-back";

        auto begin_settings = HostGraphNodeSettings(*properties, "n_begin");
        ASSERT_TRUE(begin_settings.has_value());
        auto begin_obj = begin_settings->as_object();
        ASSERT_TRUE(begin_obj.has_value());
        auto ports_arr =
            (*begin_obj)[std::string_view("boundaryPorts")].as_array();
        ASSERT_TRUE(ports_arr.has_value());
        EXPECT_EQ(ports_arr->size(), 2u)
            << "boundary edit was clobbered by the stale cached host session";
    }
    {
        auto entry_json = settings_manager_->GetTaskRepositoryEntryJson("0", 3);
        auto entry_obj = entry_json.as_object();
        ASSERT_TRUE(entry_obj.has_value());
        auto authoring =
            (*entry_obj)[std::string_view("authoring")].as_object();
        ASSERT_TRUE(authoring.has_value());
        // 宿主 revision 再推进（5 →边界 6 →宿主 apply 7）：边界写回把文档
        // version 与 authoring 域同源推进（revision 墙同源化），宿主 apply
        // 的 revision 域校验（base+1）依赖该不变量。
        EXPECT_EQ(
            (*authoring)[std::string_view("revision")].as_sint().value_or(-1),
            7);
    }
}

// begin/end 节点文档 Apply 删口级联（MaaPI 对齐管线侧）：removeFormSequence
// Item 删 p1 → 会话 diagnostics 携带被删口 id → 管线对宿主图文档执行边
// 级（removeNodePort 级联 erase 语义，双侧删）→ 不触该口的边保留。
TEST_F(SchedulerRuntimeBackedTest, BoundaryDocumentApplyRemoveItemCascadesEdges)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    SeedGraphRepositoryEntry(
        *settings_manager_,
        3,
        R"json([
            {"edgeId": "e1", "sourceNodeId": "n_begin", "sourcePortId": "p1",
             "targetNodeId": "other", "targetPortId": "in", "edgeType": "data"},
            {"edgeId": "e2", "sourceNodeId": "other", "sourcePortId": "out",
             "targetNodeId": "n_begin", "targetPortId": "p9", "edgeType": "data"}
        ])json");
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    yyjson::value change(Das::Utils::MakeYyjsonObject());
    (*change.as_object())[std::string_view("baseRevision")] =
        static_cast<int64_t>(5);
    {
        yyjson::value element(Das::Utils::MakeYyjsonObject());
        (*element.as_object())[std::string_view("kind")] = std::make_pair(
            std::string_view("removeFormSequenceItem"),
            yyjson::copy_string);
        yyjson::value payload(Das::Utils::MakeYyjsonObject());
        (*payload.as_object())[std::string_view("formSequenceItemId")] =
            std::make_pair(std::string_view("p1"), yyjson::copy_string);
        (*element.as_object())[std::string_view("payload")] =
            std::move(payload);
        auto changes = Das::Utils::MakeYyjsonArray();
        (*changes.as_array()).emplace_back(std::move(element));
        (*change.as_object())[std::string_view("changes")] = std::move(changes);
    }
    (*change.as_object())[std::string_view("componentGuid")] = std::make_pair(
        Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID,
        yyjson::copy_string);
    (*change.as_object())[std::string_view("componentContext")] = true;

    auto result = scheduler_->ApplyRepositoryEntryAuthoringChange(3, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_FALSE(obj->contains(std::string_view("errorKind")));
    EXPECT_TRUE((*obj)[std::string_view("ok")].as_bool().value_or(false));
    EXPECT_EQ((*obj)[std::string_view("revision")].as_sint().value_or(-1), 6);

    // 存储对账：e1（n_begin.p1 源端）被级联删除；e2（目标端口 p9 不在删
    // 集）保留——级联按（节点, 口）精确命中，不误删同节点其它口。
    auto properties = EntryAcceptedProperties(*settings_manager_, 3);
    ASSERT_TRUE(properties.has_value());
    {
        auto props_obj = properties->as_object();
        ASSERT_TRUE(props_obj.has_value());
        const auto& edges_ref = (*props_obj)[std::string_view("edges")];
        auto        edges_arr = edges_ref.as_array();
        ASSERT_TRUE(edges_arr.has_value());
        EXPECT_EQ(edges_arr->size(), 1u);
        auto edge_obj = (*edges_arr)[0].as_object();
        ASSERT_TRUE(edge_obj.has_value());
        EXPECT_EQ(
            (*edge_obj)[std::string_view("edgeId")].as_string().value_or(""),
            "e2");
    }
    auto begin_settings = HostGraphNodeSettings(*properties, "n_begin");
    ASSERT_TRUE(begin_settings.has_value());
    {
        auto begin_obj = begin_settings->as_object();
        ASSERT_TRUE(begin_obj.has_value());
        auto ports_arr =
            (*begin_obj)[std::string_view("boundaryPorts")].as_array();
        ASSERT_TRUE(ports_arr.has_value());
        EXPECT_EQ(ports_arr->size(), 0u);
    }
}

// 组件 kind 名与端口 label 走同一多语言 map 机制：definition 顶层 label
// map（kind 兄弟字段，kind 本身保持机器名）经 BuildActionCatalogFromRegistry
// 按进程默认 locale 解析成 entry label（回退链 locale→en→kind→guid）；
// 切换 GlobalLocale 后进程内重新 Get 即取新 locale 值（catalog 每 session
// 创建现算）。locale 值大小写归一（ZH-CN 同 zh-cn 命中）；无 label map 的
// 真实组件（das.graph.execute）entry label 回退 kind 字符串。
TEST_F(
    SchedulerRuntimeBackedTest,
    GraphAuthoringCatalogResolvesLabelMapByDefaultLocale)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    // 探针片段合并进 definition 顶层：kind 兄弟的 label map + 两个带
    // label map 的输入端口（zh-cn+en 与 en-only 两形态）。
    InjectPortDeclarationsIntoFactoryManifest(
        manifest_path_,
        FactoryTaskComponentImplGuidString,
        R"json({
            "label": {"en": "Localized Kind", "zh-cn": "本地化组件"},
            "inputs": [
                {"id": "localized_in", "type": "bool",
                 "label": {"en": "Localized In", "zh-cn": "本地化输入"}},
                {"id": "en_only_in", "type": "bool",
                 "label": {"en": "En Only In"}}
            ]
        })json");
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    // graph 任务实例：componentRef 节点指向探针组件，其端口面投影到节点
    // ports[]；entry label 从 document 级 actionCatalog.entries 读。authoring
    // session 按 (task, revision) 缓存——同 task 的后续 Get 复用创建时捕获
    // 的 context；locale 切换按重启生效语义（D-07 零刷新代码），故用三个
    // task 实例各开一个 session：每个 session 创建时按当时 locale 现算
    // catalog。
    {
        std::vector<yyjson::value> tasks;
        for (int64_t task_id = 0; task_id < 3; ++task_id)
        {
            yyjson::value task(Das::Utils::MakeYyjsonObject());
            (*task.as_object())[std::string_view("id")] = task_id;
            (*task.as_object())[std::string_view("taskGuid")] =
                "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
            (*task.as_object())[std::string_view("pluginGuid")] =
                "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
            (*task.as_object())[std::string_view("nextExecutionTime")] =
                static_cast<int64_t>(1);
            yyjson::value properties(Das::Utils::MakeYyjsonObject());
            auto          props_obj = properties.as_object();
            (*props_obj)[std::string_view("documentId")] =
                "locale-probe-graph-" + std::to_string(task_id);
            (*props_obj)[std::string_view("version")] = 1;
            yyjson::value nodes(Das::Utils::MakeYyjsonArray());
            {
                yyjson::value node(Das::Utils::MakeYyjsonObject());
                (*node.as_object())[std::string_view("nodeId")] = "probe";
                yyjson::value target(Das::Utils::MakeYyjsonObject());
                (*target.as_object())[std::string_view("targetKind")] =
                    "componentRef";
                yyjson::value component_ref(Das::Utils::MakeYyjsonObject());
                (*component_ref.as_object())[std::string_view("kind")] =
                    "componentRef";
                (*component_ref
                      .as_object())[std::string_view("componentGuid")] =
                    std::string(FactoryTaskComponentImplGuidString);
                (*component_ref.as_object())[std::string_view("pluginGuid")] =
                    std::string(FactoryPluginGuidString);
                (*target.as_object())[std::string_view("componentRef")] =
                    std::move(component_ref);
                (*node.as_object())[std::string_view("target")] =
                    std::move(target);
                (*node.as_object())[std::string_view("settings")] =
                    yyjson::value(Das::Utils::MakeYyjsonObject());
                nodes.as_array()->emplace_back(std::move(node));
            }
            (*props_obj)[std::string_view("nodes")] = std::move(nodes);
            (*props_obj)[std::string_view("edges")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            (*props_obj)[std::string_view("tags")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            (*task.as_object())[std::string_view("properties")] =
                std::move(properties);
            tasks.push_back(std::move(task));
        }
        WriteSchedulerState(*settings_manager_, 3, {0, 1, 2}, tasks);
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // 首次 Get 该 task 即创建 session（按当时 locale 现算 catalog）。
    const auto read_document_clone = [this](int64_t task_id) -> yyjson::value
    {
        const yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto result = scheduler_->GetTaskAuthoringDocument(task_id, request);
        auto obj = result.as_object();
        EXPECT_TRUE(obj.has_value());
        if (!obj.has_value() || !obj->contains(std::string_view("document")))
        {
            return yyjson::value{};
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("document")]);
    };

    // document.actionCatalog.entries 中按 actionId 取 entry 的 label。
    const auto entry_label_of = [](const yyjson::value& document_value,
                                   std::string_view action_id) -> std::string
    {
        auto document = document_value.as_object();
        if (!document.has_value()
            || !document->contains(std::string_view("actionCatalog")))
        {
            return {};
        }
        auto catalog =
            (*document)[std::string_view("actionCatalog")].as_object();
        if (!catalog.has_value()
            || !catalog->contains(std::string_view("entries")))
        {
            return {};
        }
        auto entries = (*catalog)[std::string_view("entries")].as_array();
        if (!entries.has_value())
        {
            return {};
        }
        for (const auto& entry : *entries)
        {
            auto entry_obj = entry.as_object();
            if (!entry_obj
                || !entry_obj->contains(std::string_view("actionId")))
            {
                continue;
            }
            auto id = (*entry_obj)[std::string_view("actionId")].as_string();
            if (id && *id == action_id)
            {
                return std::string((*entry_obj)[std::string_view("label")]
                                       .as_string()
                                       .value_or(""));
            }
        }
        return {};
    };

    // 节点 ports[] 中按 portId 取端口的 displayLabel。
    const auto port_label_of = [](const yyjson::value& document_value,
                                  std::string_view     port_id) -> std::string
    {
        auto document = document_value.as_object();
        if (!document.has_value()
            || !document->contains(std::string_view("graph")))
        {
            return {};
        }
        auto graph = (*document)[std::string_view("graph")].as_object();
        if (!graph.has_value() || !graph->contains(std::string_view("nodes")))
        {
            return {};
        }
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        if (!nodes.has_value())
        {
            return {};
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (!node_obj || !node_obj->contains(std::string_view("ports")))
            {
                continue;
            }
            auto ports = (*node_obj)[std::string_view("ports")].as_array();
            if (!ports.has_value())
            {
                continue;
            }
            for (const auto& port : *ports)
            {
                auto port_object = port.as_object();
                if (!port_object)
                {
                    continue;
                }
                auto id =
                    (*port_object)[std::string_view("portId")].as_string();
                if (id && *id == port_id)
                {
                    return std::string(
                        (*port_object)[std::string_view("displayLabel")]
                            .as_string()
                            .value_or(""));
                }
            }
        }
        return {};
    };

    // 默认 locale=en：entry label 与端口 label 取 map 的 en 值。
    {
        const auto document = read_document_clone(0);
        EXPECT_EQ(
            entry_label_of(document, FactoryTaskComponentImplGuidString),
            "Localized Kind");
        EXPECT_EQ(port_label_of(document, "localized_in"), "Localized In");
    }

    // zh-cn 命中：新 session 按新 locale 现算；en-only 端口回退 en 值。
    SetDefaultLocaleForTest("zh-cn");
    {
        const auto document = read_document_clone(1);
        EXPECT_EQ(
            entry_label_of(document, FactoryTaskComponentImplGuidString),
            "本地化组件");
        EXPECT_EQ(port_label_of(document, "localized_in"), "本地化输入");
        EXPECT_EQ(port_label_of(document, "en_only_in"), "En Only In");
    }

    // locale 值大小写归一：ZH-CN 同 zh-cn。
    SetDefaultLocaleForTest("ZH-CN");
    {
        const auto document = read_document_clone(2);
        EXPECT_EQ(
            entry_label_of(document, FactoryTaskComponentImplGuidString),
            "本地化组件");
        EXPECT_EQ(port_label_of(document, "localized_in"), "本地化输入");
    }

    // 真实组件守卫：das.graph.execute 的 manifest label map 已落地，ZH-CN
    // 会话解析为中文值（无 label map 组件的 kind 回退守卫由
    // GraphAuthoringActionCatalogCarriesManifestKindLabels 的探针组件断言
    // 承担）。
    EXPECT_EQ(
        entry_label_of(
            read_document_clone(2),
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3"),
        "执行子图");
}

// locale 热替换下的缓存陈旧性（读侧 locale 戳）：authoring session 缓存
// 的 label 视图烤在创建时捕获的 context 里，进程 GlobalLocale 切换后同一
// task 的后续 Get 必须拿到新语言的 label——缓存 session 因 locale 戳不等
// 而失效重建，重建消费调用方本次现算的新 context（MakeAuthoringContextJson
// → BuildActionCatalogFromRegistry 现读 locale）。此前该断言因缓存语义
// 无法在同 task 上成立（只能靠多 task 实例各开 session 规避），locale 戳
// 落地后原语义回归；同 locale 连续 Get 仍复用 session（locale 戳与
// revision 戳都不变 → 命中复用分支；复用的计数器级证明由
// SchedulerAuthoringTaskCapabilityRegistryGetDocument 家族的
// authoring_session_count==1 断言承担）。locale 的保存/还原由夹具
// saved_locale_ 负责。
TEST_F(
    SchedulerRuntimeBackedTest,
    GraphAuthoringCatalogRebuildsSessionOnLocaleChange)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    InjectPortDeclarationsIntoFactoryManifest(
        manifest_path_,
        FactoryTaskComponentImplGuidString,
        R"json({
            "label": {"en": "Localized Kind", "zh-cn": "本地化组件"},
            "inputs": [
                {"id": "localized_in", "type": "bool",
                 "label": {"en": "Localized In", "zh-cn": "本地化输入"}}
            ]
        })json");
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    // 单个 graph 任务实例：locale 切换前后的 Get 都打在同一 task id 上，
    // 直面缓存 session 的陈旧性（多实例规避会绕开被测语义）。
    {
        yyjson::value task(Das::Utils::MakeYyjsonObject());
        (*task.as_object())[std::string_view("id")] = static_cast<int64_t>(0);
        (*task.as_object())[std::string_view("taskGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        (*task.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*task.as_object())[std::string_view("nextExecutionTime")] =
            static_cast<int64_t>(1);
        yyjson::value properties(Das::Utils::MakeYyjsonObject());
        auto          props_obj = properties.as_object();
        (*props_obj)[std::string_view("documentId")] = "locale-rebuild-graph-0";
        (*props_obj)[std::string_view("version")] = 1;
        yyjson::value nodes(Das::Utils::MakeYyjsonArray());
        {
            yyjson::value node(Das::Utils::MakeYyjsonObject());
            (*node.as_object())[std::string_view("nodeId")] = "probe";
            yyjson::value target(Das::Utils::MakeYyjsonObject());
            (*target.as_object())[std::string_view("targetKind")] =
                "componentRef";
            yyjson::value component_ref(Das::Utils::MakeYyjsonObject());
            (*component_ref.as_object())[std::string_view("kind")] =
                "componentRef";
            (*component_ref.as_object())[std::string_view("componentGuid")] =
                std::string(FactoryTaskComponentImplGuidString);
            (*component_ref.as_object())[std::string_view("pluginGuid")] =
                std::string(FactoryPluginGuidString);
            (*target.as_object())[std::string_view("componentRef")] =
                std::move(component_ref);
            (*node.as_object())[std::string_view("target")] = std::move(target);
            (*node.as_object())[std::string_view("settings")] =
                yyjson::value(Das::Utils::MakeYyjsonObject());
            nodes.as_array()->emplace_back(std::move(node));
        }
        (*props_obj)[std::string_view("nodes")] = std::move(nodes);
        (*props_obj)[std::string_view("edges")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*props_obj)[std::string_view("tags")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*task.as_object())[std::string_view("properties")] =
            std::move(properties);
        WriteSchedulerState(*settings_manager_, 1, {0}, {task});
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const auto read_document_clone = [this]() -> yyjson::value
    {
        const yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto result = scheduler_->GetTaskAuthoringDocument(0, request);
        auto obj = result.as_object();
        EXPECT_TRUE(obj.has_value());
        if (!obj.has_value() || !obj->contains(std::string_view("document")))
        {
            return yyjson::value{};
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("document")]);
    };

    const auto entry_label_of = [](const yyjson::value& document_value,
                                   std::string_view action_id) -> std::string
    {
        auto document = document_value.as_object();
        if (!document.has_value()
            || !document->contains(std::string_view("actionCatalog")))
        {
            return {};
        }
        auto catalog =
            (*document)[std::string_view("actionCatalog")].as_object();
        if (!catalog.has_value()
            || !catalog->contains(std::string_view("entries")))
        {
            return {};
        }
        auto entries = (*catalog)[std::string_view("entries")].as_array();
        if (!entries.has_value())
        {
            return {};
        }
        for (const auto& entry : *entries)
        {
            auto entry_obj = entry.as_object();
            if (!entry_obj
                || !entry_obj->contains(std::string_view("actionId")))
            {
                continue;
            }
            auto id = (*entry_obj)[std::string_view("actionId")].as_string();
            if (id && *id == action_id)
            {
                return std::string((*entry_obj)[std::string_view("label")]
                                       .as_string()
                                       .value_or(""));
            }
        }
        return {};
    };

    const auto port_label_of = [](const yyjson::value& document_value,
                                  std::string_view     port_id) -> std::string
    {
        auto document = document_value.as_object();
        if (!document.has_value()
            || !document->contains(std::string_view("graph")))
        {
            return {};
        }
        auto graph = (*document)[std::string_view("graph")].as_object();
        if (!graph.has_value() || !graph->contains(std::string_view("nodes")))
        {
            return {};
        }
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        if (!nodes.has_value())
        {
            return {};
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (!node_obj || !node_obj->contains(std::string_view("ports")))
            {
                continue;
            }
            auto ports = (*node_obj)[std::string_view("ports")].as_array();
            if (!ports.has_value())
            {
                continue;
            }
            for (const auto& port : *ports)
            {
                auto port_object = port.as_object();
                if (!port_object)
                {
                    continue;
                }
                auto id =
                    (*port_object)[std::string_view("portId")].as_string();
                if (id && *id == port_id)
                {
                    return std::string(
                        (*port_object)[std::string_view("displayLabel")]
                            .as_string()
                            .value_or(""));
                }
            }
        }
        return {};
    };

    // 默认 locale=en：首次 Get 创建 session，label 取 map 的 en 值。
    {
        const auto document = read_document_clone();
        EXPECT_EQ(
            entry_label_of(document, FactoryTaskComponentImplGuidString),
            "Localized Kind");
        EXPECT_EQ(port_label_of(document, "localized_in"), "Localized In");
    }

    // 同 locale 二次 Get：语义结果与首次一致（locale/revision 戳均未变，
    // 命中复用分支）。
    {
        const auto document = read_document_clone();
        EXPECT_EQ(
            entry_label_of(document, FactoryTaskComponentImplGuidString),
            "Localized Kind");
        EXPECT_EQ(port_label_of(document, "localized_in"), "Localized In");
    }

    // locale 热切换到 zh-cn 后同一 task 的 Get：缓存 session 的 locale 戳
    // 不等 → 失效重建，新 session 按新 context 现算，label 换中文。
    SetDefaultLocaleForTest("zh-cn");
    {
        const auto document = read_document_clone();
        EXPECT_EQ(
            entry_label_of(document, FactoryTaskComponentImplGuidString),
            "本地化组件");
        EXPECT_EQ(port_label_of(document, "localized_in"), "本地化输入");
    }
}

// 真实 manifest 文案全链本地化（DasFlowControl 与 DasGraphTask 的 label
// map 落地后）：默认 locale=en 时 entry label 与节点端口 displayLabel 均
// 取 manifest map 的 en 值；GlobalLocale 切到 zh-cn（测试内模拟重启后
// locale——D-07 重启生效语义）后新 session 现算取中文值。authoring
// session 按 (task, revision) 缓存，同 task 二次 Get 复用旧 context，
// 故用两个 task 实例各开一个 session。
TEST_F(
    SchedulerRuntimeBackedTest,
    GraphAuthoringCatalogLocalizesRealManifestLabels)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);
    DeployRealPluginArtifacts(
        "libDasFlowControl.dll",
        "DasFlowControl.json",
        /*force_ipc=*/false,
        "DasFlowControl");

    // 两个 graph 任务实例：componentRef 节点指向 das.flow.merge，其静态
    // 端口面（in_true/…/result）投影到节点 ports[]；entry label 从
    // document 级 actionCatalog.entries 读。
    {
        std::vector<yyjson::value> tasks;
        for (int64_t task_id = 0; task_id < 2; ++task_id)
        {
            yyjson::value task(Das::Utils::MakeYyjsonObject());
            (*task.as_object())[std::string_view("id")] = task_id;
            (*task.as_object())[std::string_view("taskGuid")] =
                "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
            (*task.as_object())[std::string_view("pluginGuid")] =
                "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
            (*task.as_object())[std::string_view("nextExecutionTime")] =
                static_cast<int64_t>(1);
            yyjson::value properties(Das::Utils::MakeYyjsonObject());
            auto          props_obj = properties.as_object();
            (*props_obj)[std::string_view("documentId")] =
                "real-manifest-graph-" + std::to_string(task_id);
            (*props_obj)[std::string_view("version")] = 1;
            yyjson::value nodes(Das::Utils::MakeYyjsonArray());
            {
                yyjson::value node(Das::Utils::MakeYyjsonObject());
                (*node.as_object())[std::string_view("nodeId")] = "merge";
                yyjson::value target(Das::Utils::MakeYyjsonObject());
                (*target.as_object())[std::string_view("targetKind")] =
                    "componentRef";
                yyjson::value component_ref(Das::Utils::MakeYyjsonObject());
                (*component_ref.as_object())[std::string_view("kind")] =
                    "componentRef";
                (*component_ref
                      .as_object())[std::string_view("componentGuid")] =
                    "68F10008-0000-4000-8000-000000000008";
                (*component_ref.as_object())[std::string_view("pluginGuid")] =
                    "7115BBFA-7E57-494E-82D5-D4134C367E13";
                (*target.as_object())[std::string_view("componentRef")] =
                    std::move(component_ref);
                (*node.as_object())[std::string_view("target")] =
                    std::move(target);
                (*node.as_object())[std::string_view("settings")] =
                    yyjson::value(Das::Utils::MakeYyjsonObject());
                nodes.as_array()->emplace_back(std::move(node));
            }
            (*props_obj)[std::string_view("nodes")] = std::move(nodes);
            (*props_obj)[std::string_view("edges")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            (*props_obj)[std::string_view("tags")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            (*task.as_object())[std::string_view("properties")] =
                std::move(properties);
            tasks.push_back(std::move(task));
        }
        WriteSchedulerState(*settings_manager_, 2, {0, 1}, tasks);
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // 首次 Get 该 task 即创建 session（按当时 locale 现算 catalog）。
    const auto read_document_clone = [this](int64_t task_id) -> yyjson::value
    {
        const yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto result = scheduler_->GetTaskAuthoringDocument(task_id, request);
        auto obj = result.as_object();
        EXPECT_TRUE(obj.has_value());
        if (!obj.has_value() || !obj->contains(std::string_view("document")))
        {
            return yyjson::value{};
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("document")]);
    };

    // document.actionCatalog.entries 中按 actionId 取 entry 的 label。
    const auto entry_label_of = [](const yyjson::value& document_value,
                                   std::string_view action_id) -> std::string
    {
        auto document = document_value.as_object();
        if (!document.has_value()
            || !document->contains(std::string_view("actionCatalog")))
        {
            return {};
        }
        auto catalog =
            (*document)[std::string_view("actionCatalog")].as_object();
        if (!catalog.has_value()
            || !catalog->contains(std::string_view("entries")))
        {
            return {};
        }
        auto entries = (*catalog)[std::string_view("entries")].as_array();
        if (!entries.has_value())
        {
            return {};
        }
        for (const auto& entry : *entries)
        {
            auto entry_obj = entry.as_object();
            if (!entry_obj
                || !entry_obj->contains(std::string_view("actionId")))
            {
                continue;
            }
            auto id = (*entry_obj)[std::string_view("actionId")].as_string();
            if (id && *id == action_id)
            {
                return std::string((*entry_obj)[std::string_view("label")]
                                       .as_string()
                                       .value_or(""));
            }
        }
        return {};
    };

    // 节点 ports[] 中按 portId 取端口的 displayLabel。
    const auto port_label_of = [](const yyjson::value& document_value,
                                  std::string_view     port_id) -> std::string
    {
        auto document = document_value.as_object();
        if (!document.has_value()
            || !document->contains(std::string_view("graph")))
        {
            return {};
        }
        auto graph = (*document)[std::string_view("graph")].as_object();
        if (!graph.has_value() || !graph->contains(std::string_view("nodes")))
        {
            return {};
        }
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        if (!nodes.has_value())
        {
            return {};
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (!node_obj || !node_obj->contains(std::string_view("ports")))
            {
                continue;
            }
            auto ports = (*node_obj)[std::string_view("ports")].as_array();
            if (!ports.has_value())
            {
                continue;
            }
            for (const auto& port : *ports)
            {
                auto port_object = port.as_object();
                if (!port_object)
                {
                    continue;
                }
                auto id =
                    (*port_object)[std::string_view("portId")].as_string();
                if (id && *id == port_id)
                {
                    return std::string(
                        (*port_object)[std::string_view("displayLabel")]
                            .as_string()
                            .value_or(""));
                }
            }
        }
        return {};
    };

    // 默认 locale=en：真实 manifest 的 en 值经调度器→context→插件会话→
    // Core 投影全链可见。
    {
        const auto document = read_document_clone(0);
        EXPECT_EQ(
            entry_label_of(document, "68F10008-0000-4000-8000-000000000008"),
            "Merge");
        EXPECT_EQ(
            entry_label_of(document, "95FD2FFA-88F2-4F85-A536-C708D25B641F"),
            "Launch Process");
        EXPECT_EQ(
            entry_label_of(document, "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3"),
            "Execute Graph");
        EXPECT_EQ(port_label_of(document, "in_true"), "True Input");
        EXPECT_EQ(port_label_of(document, "result"), "Result");
    }

    // zh-cn：真实 manifest 的中文值（模拟重启后 locale 的 catalog 重建）。
    SetDefaultLocaleForTest("zh-cn");
    {
        const auto document = read_document_clone(1);
        EXPECT_EQ(
            entry_label_of(document, "68F10008-0000-4000-8000-000000000008"),
            "合并");
        EXPECT_EQ(
            entry_label_of(document, "95FD2FFA-88F2-4F85-A536-C708D25B641F"),
            "启动进程");
        EXPECT_EQ(
            entry_label_of(document, "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3"),
            "执行子图");
        EXPECT_EQ(port_label_of(document, "in_true"), "真值输入");
        EXPECT_EQ(port_label_of(document, "result"), "结果");
    }
}

// config 字段文案全链本地化（manifest config 条目的 label map 落地后）：节点
// 编辑窗 fields[] 的 label 不再是裸机器名——catalog 生产侧把 config 条目的
// label map 按进程默认 locale 解析成字符串随通道下发（engaged-only），Core
// 字段投影以其为显示文案（无声明才回退字段名）。默认 locale=en 取 map 的
// en 值；GlobalLocale 切到 zh-cn（测试内模拟重启后 locale）新 session 现算
// 取中文值。authoring session 按 (task, revision) 缓存，同 task 二次 Get
// 复用旧 context，故用两个 task 实例各开一个 session。
TEST_F(
    SchedulerRuntimeBackedTest,
    GraphAuthoringCatalogLocalizesRealManifestConfigFieldLabels)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);
    DeployRealPluginArtifacts(
        "libDasFlowControl.dll",
        "DasFlowControl.json",
        /*force_ipc=*/false,
        "DasFlowControl");

    // 两个 graph 任务实例：componentRef 节点指向 das.flow.launch（6 个
    // config 声明字段），其字段面投影到节点 fields[]（graph 视图只含
    // config 声明）。
    {
        std::vector<yyjson::value> tasks;
        for (int64_t task_id = 0; task_id < 2; ++task_id)
        {
            yyjson::value task(Das::Utils::MakeYyjsonObject());
            (*task.as_object())[std::string_view("id")] = task_id;
            (*task.as_object())[std::string_view("taskGuid")] =
                "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
            (*task.as_object())[std::string_view("pluginGuid")] =
                "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
            (*task.as_object())[std::string_view("nextExecutionTime")] =
                static_cast<int64_t>(1);
            yyjson::value properties(Das::Utils::MakeYyjsonObject());
            auto          props_obj = properties.as_object();
            (*props_obj)[std::string_view("documentId")] =
                "config-label-graph-" + std::to_string(task_id);
            (*props_obj)[std::string_view("version")] = 1;
            yyjson::value nodes(Das::Utils::MakeYyjsonArray());
            {
                yyjson::value node(Das::Utils::MakeYyjsonObject());
                (*node.as_object())[std::string_view("nodeId")] = "launch";
                yyjson::value target(Das::Utils::MakeYyjsonObject());
                (*target.as_object())[std::string_view("targetKind")] =
                    "componentRef";
                yyjson::value component_ref(Das::Utils::MakeYyjsonObject());
                (*component_ref.as_object())[std::string_view("kind")] =
                    "componentRef";
                (*component_ref
                      .as_object())[std::string_view("componentGuid")] =
                    "95FD2FFA-88F2-4F85-A536-C708D25B641F";
                (*component_ref.as_object())[std::string_view("pluginGuid")] =
                    "7115BBFA-7E57-494E-82D5-D4134C367E13";
                (*target.as_object())[std::string_view("componentRef")] =
                    std::move(component_ref);
                (*node.as_object())[std::string_view("target")] =
                    std::move(target);
                (*node.as_object())[std::string_view("settings")] =
                    yyjson::value(Das::Utils::MakeYyjsonObject());
                nodes.as_array()->emplace_back(std::move(node));
            }
            (*props_obj)[std::string_view("nodes")] = std::move(nodes);
            (*props_obj)[std::string_view("edges")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            (*props_obj)[std::string_view("tags")] =
                yyjson::value(Das::Utils::MakeYyjsonArray());
            (*task.as_object())[std::string_view("properties")] =
                std::move(properties);
            tasks.push_back(std::move(task));
        }
        WriteSchedulerState(*settings_manager_, 2, {0, 1}, tasks);
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // 首次 Get 该 task 即创建 session（按当时 locale 现算 catalog）。
    const auto read_document_clone = [this](int64_t task_id) -> yyjson::value
    {
        const yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto result = scheduler_->GetTaskAuthoringDocument(task_id, request);
        auto obj = result.as_object();
        EXPECT_TRUE(obj.has_value());
        if (!obj.has_value() || !obj->contains(std::string_view("document")))
        {
            return yyjson::value{};
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("document")]);
    };

    // 节点 fields[] 中按 name 取字段的 label。
    const auto field_label_of = [](const yyjson::value& document_value,
                                   std::string_view field_name) -> std::string
    {
        auto document = document_value.as_object();
        if (!document.has_value()
            || !document->contains(std::string_view("graph")))
        {
            return {};
        }
        auto graph = (*document)[std::string_view("graph")].as_object();
        if (!graph.has_value() || !graph->contains(std::string_view("nodes")))
        {
            return {};
        }
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        if (!nodes.has_value())
        {
            return {};
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (!node_obj || !node_obj->contains(std::string_view("fields")))
            {
                continue;
            }
            auto fields = (*node_obj)[std::string_view("fields")].as_array();
            if (!fields.has_value())
            {
                continue;
            }
            for (const auto& field : *fields)
            {
                auto field_object = field.as_object();
                if (!field_object)
                {
                    continue;
                }
                auto name =
                    (*field_object)[std::string_view("name")].as_string();
                if (name && *name == field_name)
                {
                    return std::string(
                        (*field_object)[std::string_view("label")]
                            .as_string()
                            .value_or(""));
                }
            }
        }
        return {};
    };

    // 节点 fields[] 中按 name 取字段的 description（engaged-only：未声明
    // 的字段无该键，返回空串）。
    const auto field_description_of =
        [](const yyjson::value& document_value,
           std::string_view     field_name) -> std::string
    {
        auto document = document_value.as_object();
        if (!document.has_value()
            || !document->contains(std::string_view("graph")))
        {
            return {};
        }
        auto graph = (*document)[std::string_view("graph")].as_object();
        if (!graph.has_value() || !graph->contains(std::string_view("nodes")))
        {
            return {};
        }
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        if (!nodes.has_value())
        {
            return {};
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (!node_obj || !node_obj->contains(std::string_view("fields")))
            {
                continue;
            }
            auto fields = (*node_obj)[std::string_view("fields")].as_array();
            if (!fields.has_value())
            {
                continue;
            }
            for (const auto& field : *fields)
            {
                auto field_object = field.as_object();
                if (!field_object)
                {
                    continue;
                }
                auto name =
                    (*field_object)[std::string_view("name")].as_string();
                if (name && *name == field_name)
                {
                    if (!field_object->contains(
                            std::string_view("description")))
                    {
                        return {};
                    }
                    return std::string(
                        (*field_object)[std::string_view("description")]
                            .as_string()
                            .value_or(""));
                }
            }
        }
        return {};
    };

    // 默认 locale=en：真实 manifest 的 en 值经调度器→context→插件会话→
    // Core 字段投影全链可见。
    {
        const auto document = read_document_clone(0);
        EXPECT_EQ(field_label_of(document, "exePath"), "Executable Path");
        EXPECT_EQ(field_label_of(document, "args"), "Arguments");
        EXPECT_EQ(
            field_label_of(document, "workingDirectory"),
            "Working Directory");
        EXPECT_EQ(
            field_label_of(document, "waitWindowClassRegex"),
            "Wait Window: Class Regex");
        EXPECT_EQ(
            field_label_of(document, "waitWindowWindowRegex"),
            "Wait Window: Title Regex");
        EXPECT_EQ(
            field_label_of(document, "waitWindowTimeoutMs"),
            "Wait Window Timeout (ms)");
        // description 同链可见（engaged-only；未声明 exePath 无该键）。
        EXPECT_EQ(field_description_of(document, "exePath"), "");
        EXPECT_TRUE(field_description_of(document, "waitWindowClassRegex")
                        .starts_with("When set, the node blocks"));
        EXPECT_TRUE(field_description_of(document, "waitWindowWindowRegex")
                        .starts_with("When set, the node blocks"));
        EXPECT_TRUE(field_description_of(document, "waitWindowTimeoutMs")
                        .starts_with("Maximum time to wait"));
    }

    // zh-cn：真实 manifest 的中文值（模拟重启后 locale 的 catalog 重建）。
    SetDefaultLocaleForTest("zh-cn");
    {
        const auto document = read_document_clone(1);
        EXPECT_EQ(field_label_of(document, "exePath"), "可执行文件路径");
        EXPECT_EQ(field_label_of(document, "args"), "启动参数");
        EXPECT_EQ(field_label_of(document, "workingDirectory"), "工作目录");
        EXPECT_EQ(
            field_label_of(document, "waitWindowClassRegex"),
            "等待窗口：类名正则");
        EXPECT_EQ(
            field_label_of(document, "waitWindowWindowRegex"),
            "等待窗口：标题正则");
        EXPECT_EQ(
            field_label_of(document, "waitWindowTimeoutMs"),
            "等待窗口超时（毫秒）");
        EXPECT_EQ(field_description_of(document, "exePath"), "");
        EXPECT_TRUE(field_description_of(document, "waitWindowClassRegex")
                        .starts_with("填写后，节点在启动进程后会阻塞等待"));
        EXPECT_TRUE(field_description_of(document, "waitWindowWindowRegex")
                        .starts_with("填写后，节点在启动进程后会阻塞等待"));
        EXPECT_TRUE(field_description_of(document, "waitWindowTimeoutMs")
                        .starts_with("等待目标窗口出现的最长时长"));
    }
}

// manifest label map 键大小写无关命中：BCP47 语言标签的规范书写是语言
// 小写、地区大写（"zh-CN"），第三方 manifest 作者按标准习惯写键名时，
// zh-cn 查询（查询侧与键侧双侧 toLower 后比较）必须命中该键的值，不得
// 静默回退 en 值。组件级（kind 兄弟的 label map）与端口级两个查表点
// 各自断言。
TEST_F(
    SchedulerRuntimeBackedTest,
    GraphAuthoringCatalogMatchesBcp47CasedLocaleKeys)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    // 探针片段：组件级与端口级 label map 的键都用 BCP47 规范大小写
    // "zh-CN"（非小写）书写。
    InjectPortDeclarationsIntoFactoryManifest(
        manifest_path_,
        FactoryTaskComponentImplGuidString,
        R"json({
            "label": {"en": "Cased Fallback Kind", "zh-CN": "规范键组件"},
            "inputs": [
                {"id": "cased_in", "type": "bool",
                 "label": {"en": "Cased Fallback In", "zh-CN": "规范键输入"}}
            ]
        })json");
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    // 单个 graph 任务实例：componentRef 节点指向探针组件，其端口面投影
    // 到节点 ports[]；entry label 从 document 级 actionCatalog.entries 读。
    {
        yyjson::value task0(Das::Utils::MakeYyjsonObject());
        (*task0.as_object())[std::string_view("id")] = static_cast<int64_t>(0);
        (*task0.as_object())[std::string_view("taskGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        (*task0.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*task0.as_object())[std::string_view("nextExecutionTime")] =
            static_cast<int64_t>(1);
        yyjson::value properties(Das::Utils::MakeYyjsonObject());
        auto          props_obj = properties.as_object();
        (*props_obj)[std::string_view("documentId")] = "bcp47-key-graph";
        (*props_obj)[std::string_view("version")] = 1;
        yyjson::value nodes(Das::Utils::MakeYyjsonArray());
        {
            yyjson::value node(Das::Utils::MakeYyjsonObject());
            (*node.as_object())[std::string_view("nodeId")] = "probe";
            yyjson::value target(Das::Utils::MakeYyjsonObject());
            (*target.as_object())[std::string_view("targetKind")] =
                "componentRef";
            yyjson::value component_ref(Das::Utils::MakeYyjsonObject());
            (*component_ref.as_object())[std::string_view("kind")] =
                "componentRef";
            (*component_ref.as_object())[std::string_view("componentGuid")] =
                FactoryTaskComponentImplGuidString;
            (*component_ref.as_object())[std::string_view("pluginGuid")] =
                FactoryPluginGuidString;
            (*target.as_object())[std::string_view("componentRef")] =
                std::move(component_ref);
            (*node.as_object())[std::string_view("target")] = std::move(target);
            (*node.as_object())[std::string_view("settings")] =
                yyjson::value(Das::Utils::MakeYyjsonObject());
            nodes.as_array()->emplace_back(std::move(node));
        }
        (*props_obj)[std::string_view("nodes")] = std::move(nodes);
        (*props_obj)[std::string_view("edges")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*props_obj)[std::string_view("tags")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*task0.as_object())[std::string_view("properties")] =
            std::move(properties);
        std::vector<yyjson::value> tasks;
        tasks.push_back(std::move(task0));
        WriteSchedulerState(*settings_manager_, 1, {0}, tasks);
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // 首次 Get 该 task 即创建 session（按当时 locale 现算 catalog）；
    // zh-cn 查询必须命中 "zh-CN" 键——若键侧不归一，会静默回退 en 值，
    // 下面两条断言都会失败。
    SetDefaultLocaleForTest("zh-cn");
    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto result = scheduler_->GetTaskAuthoringDocument(0, request);
    auto result_obj = result.as_object();
    ASSERT_TRUE(result_obj.has_value());
    ASSERT_TRUE(result_obj->contains(std::string_view("document")));
    const auto document = Das::Utils::CloneYyjsonValue(
        (*result_obj)[std::string_view("document")]);

    // document.actionCatalog.entries 中按 actionId 取 entry 的 label。
    const auto entry_label_of = [](const yyjson::value& document_value,
                                   std::string_view action_id) -> std::string
    {
        auto document = document_value.as_object();
        if (!document.has_value()
            || !document->contains(std::string_view("actionCatalog")))
        {
            return {};
        }
        auto catalog =
            (*document)[std::string_view("actionCatalog")].as_object();
        if (!catalog.has_value()
            || !catalog->contains(std::string_view("entries")))
        {
            return {};
        }
        auto entries = (*catalog)[std::string_view("entries")].as_array();
        if (!entries.has_value())
        {
            return {};
        }
        for (const auto& entry : *entries)
        {
            auto entry_obj = entry.as_object();
            if (!entry_obj
                || !entry_obj->contains(std::string_view("actionId")))
            {
                continue;
            }
            auto id = (*entry_obj)[std::string_view("actionId")].as_string();
            if (id && *id == action_id)
            {
                return std::string((*entry_obj)[std::string_view("label")]
                                       .as_string()
                                       .value_or(""));
            }
        }
        return {};
    };

    // 节点 ports[] 中按 portId 取端口的 displayLabel。
    const auto port_label_of = [](const yyjson::value& document_value,
                                  std::string_view     port_id) -> std::string
    {
        auto document = document_value.as_object();
        if (!document.has_value()
            || !document->contains(std::string_view("graph")))
        {
            return {};
        }
        auto graph = (*document)[std::string_view("graph")].as_object();
        if (!graph.has_value() || !graph->contains(std::string_view("nodes")))
        {
            return {};
        }
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        if (!nodes.has_value())
        {
            return {};
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (!node_obj || !node_obj->contains(std::string_view("ports")))
            {
                continue;
            }
            auto ports = (*node_obj)[std::string_view("ports")].as_array();
            if (!ports.has_value())
            {
                continue;
            }
            for (const auto& port : *ports)
            {
                auto port_object = port.as_object();
                if (!port_object)
                {
                    continue;
                }
                auto id =
                    (*port_object)[std::string_view("portId")].as_string();
                if (id && *id == port_id)
                {
                    return std::string(
                        (*port_object)[std::string_view("displayLabel")]
                            .as_string()
                            .value_or(""));
                }
            }
        }
        return {};
    };

    EXPECT_EQ(
        entry_label_of(document, FactoryTaskComponentImplGuidString),
        "规范键组件");
    EXPECT_EQ(port_label_of(document, "cased_in"), "规范键输入");
}

// 组件 manifest config 声明 → 注册表 catalog 通道 → 节点 fields[] 投影全
// 链：delay_ms（int64，default 1000）与 repositoryRef（taskRepositoryRef →
// valueType 16 / inputType 0 + wire 携带 config 类型词与声明的 taskGuid）在
// 新建节点（settings 空）上即可见可编辑。
TEST_F(
    SchedulerRuntimeBackedTest,
    GraphAuthoringCatalogProjectsConfigDefaultsIntoFields)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        true,
        "Cpp",
        FactoryTaskComponentImplGuidString,
        /*include_flow_components=*/false);
    InjectPortDeclarationsIntoFactoryManifest(
        manifest_path_,
        FactoryTaskComponentImplGuidString,
        R"json({
            "config": {
                "delay_ms": {"type": "int64", "default": 1000},
                "repositoryRef": {
                    "type": "taskRepositoryRef",
                    "taskGuid": "69F20001-0000-4000-8000-000000000001"
                }
            }
        })json");
    DeployRealPluginArtifacts(
        "libDasGraphTask.dll",
        "DasGraphTask.json",
        /*force_ipc=*/true);

    // graph 任务实例：properties 携带一个 componentRef 节点指向探针组件，
    // settings 为空——config 声明的默认值应直接进入 fields[]。
    {
        yyjson::value task0(Das::Utils::MakeYyjsonObject());
        (*task0.as_object())[std::string_view("id")] = 0;
        (*task0.as_object())[std::string_view("taskGuid")] =
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        (*task0.as_object())[std::string_view("pluginGuid")] =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        (*task0.as_object())[std::string_view("nextExecutionTime")] =
            static_cast<int64_t>(1);
        yyjson::value properties(Das::Utils::MakeYyjsonObject());
        auto          props_obj = properties.as_object();
        (*props_obj)[std::string_view("documentId")] = "config-probe-graph";
        (*props_obj)[std::string_view("version")] = 1;
        yyjson::value nodes(Das::Utils::MakeYyjsonArray());
        {
            yyjson::value node(Das::Utils::MakeYyjsonObject());
            (*node.as_object())[std::string_view("nodeId")] = "probe";
            yyjson::value target(Das::Utils::MakeYyjsonObject());
            (*target.as_object())[std::string_view("targetKind")] =
                "componentRef";
            yyjson::value component_ref(Das::Utils::MakeYyjsonObject());
            (*component_ref.as_object())[std::string_view("kind")] =
                "componentRef";
            (*component_ref.as_object())[std::string_view("componentGuid")] =
                std::string(FactoryTaskComponentImplGuidString);
            (*component_ref.as_object())[std::string_view("pluginGuid")] =
                std::string(FactoryPluginGuidString);
            (*target.as_object())[std::string_view("componentRef")] =
                std::move(component_ref);
            (*node.as_object())[std::string_view("target")] = std::move(target);
            (*node.as_object())[std::string_view("settings")] =
                yyjson::value(Das::Utils::MakeYyjsonObject());
            nodes.as_array()->emplace_back(std::move(node));
        }
        (*props_obj)[std::string_view("nodes")] = std::move(nodes);
        (*props_obj)[std::string_view("edges")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*props_obj)[std::string_view("tags")] =
            yyjson::value(Das::Utils::MakeYyjsonArray());
        (*task0.as_object())[std::string_view("properties")] =
            std::move(properties);
        WriteSchedulerState(*settings_manager_, 1, {0}, {task0});
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto result = scheduler_->GetTaskAuthoringDocument(0, request);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE(obj->contains(std::string_view("document")));
    auto document = (*obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    ASSERT_TRUE(document->contains(std::string_view("graph")));
    auto graph = (*document)[std::string_view("graph")].as_object();
    ASSERT_TRUE(graph.has_value());
    ASSERT_TRUE(graph->contains(std::string_view("nodes")));
    auto nodes = (*graph)[std::string_view("nodes")].as_array();
    ASSERT_TRUE(nodes.has_value());
    ASSERT_EQ(nodes->size(), 1u);
    auto node_obj = (*nodes)[0].as_object();
    ASSERT_TRUE(node_obj.has_value());
    ASSERT_TRUE(node_obj->contains(std::string_view("fields")));
    auto fields = (*node_obj)[std::string_view("fields")].as_array();
    ASSERT_TRUE(fields.has_value());

    // 字段查找替身：按 name 定位并返回字段对象的克隆。
    const auto field_of =
        [&fields](std::string_view field_name) -> std::optional<yyjson::value>
    {
        for (const auto& field : *fields)
        {
            auto field_object = field.as_object();
            if (!field_object)
            {
                continue;
            }
            auto name = (*field_object)[std::string_view("name")].as_string();
            if (name && *name == field_name)
            {
                return Das::Utils::CloneYyjsonValue(field);
            }
        }
        return std::nullopt;
    };
    const auto integer_field =
        [](const yyjson::value& field_value, std::string_view key)
    {
        auto field_object = field_value.as_object();
        if (!field_object)
        {
            return std::optional<int64_t>{};
        }
        return (*field_object)[key].as_int();
    };

    // 声明序（字典序）先出：delay_ms 的默认值可见可编辑。
    const auto delay_field = field_of("delay_ms");
    ASSERT_TRUE(delay_field.has_value());
    ASSERT_EQ(fields->size() >= 2u, true);
    EXPECT_EQ(
        (*fields)[0]
            .as_object()
            .value()[std::string_view("name")]
            .as_string()
            .value(),
        std::string_view("delay_ms"));
    auto delay_value = integer_field(*delay_field, "value");
    ASSERT_TRUE(delay_value.has_value());
    EXPECT_EQ(*delay_value, 1000);
    auto delay_value_type = integer_field(*delay_field, "valueType");
    ASSERT_TRUE(delay_value_type.has_value());
    EXPECT_EQ(*delay_value_type, 0);
    auto delay_input_type = integer_field(*delay_field, "inputType");
    ASSERT_TRUE(delay_input_type.has_value());
    EXPECT_EQ(*delay_input_type, 0);

    // taskRepositoryRef → valueType 16 / inputType 0（保守 object；分派走
    // 字段 config 类型词 "type"，inputType 官方词表 0-8 不扩词），无默认
    // 无设置 → value null（选择器从空开始）。
    const auto ref_field = field_of("repositoryRef");
    ASSERT_TRUE(ref_field.has_value());
    auto ref_value_type = integer_field(*ref_field, "valueType");
    ASSERT_TRUE(ref_value_type.has_value());
    EXPECT_EQ(*ref_value_type, 16);
    auto ref_input_type = integer_field(*ref_field, "inputType");
    ASSERT_TRUE(ref_input_type.has_value());
    EXPECT_EQ(*ref_input_type, 0);
    ASSERT_TRUE((*ref_field->as_object())[std::string_view("value")].is_null());

    // config 类型词随 wire 字段投影携带（声明字段才有）；声明的目标任务
    // 类型（taskGuid）同样 engaged-only 携带——未声明的字段不带该键。
    auto ref_field_object = ref_field->as_object();
    ASSERT_TRUE(ref_field_object.has_value());
    ASSERT_TRUE(ref_field_object->contains(std::string_view("type")));
    EXPECT_EQ(
        (*ref_field_object)[std::string_view("type")].as_string().value_or(""),
        "taskRepositoryRef");
    ASSERT_TRUE(ref_field_object->contains(std::string_view("taskGuid")));
    EXPECT_EQ(
        (*ref_field_object)[std::string_view("taskGuid")].as_string().value_or(
            ""),
        "69F20001-0000-4000-8000-000000000001");
    const auto delay_field_object = delay_field->as_object();
    ASSERT_TRUE(delay_field_object.has_value());
    ASSERT_TRUE(delay_field_object->contains(std::string_view("type")));
    EXPECT_EQ(
        (*delay_field_object)[std::string_view("type")].as_string().value_or(
            ""),
        "int64");
    EXPECT_FALSE(delay_field_object->contains(std::string_view("taskGuid")));

    // 无 label map 的 config 字段：fields[] 的 label 回退字段名——label 通道
    // 引入后无声明字段的投影行为不变（回退链锁定）。
    EXPECT_EQ(
        (*delay_field_object)[std::string_view("label")].as_string().value_or(
            ""),
        "delay_ms");
    EXPECT_EQ(
        (*ref_field_object)[std::string_view("label")].as_string().value_or(""),
        "repositoryRef");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryCreateReturnsEntryFromDescriptorDefaults)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    yyjson::value request(Das::Utils::MakeYyjsonObject());
    (*request.as_object())[std::string_view("pluginGuid")] =
        FactoryPluginGuidString;
    (*request.as_object())[std::string_view("taskTypeGuid")] =
        FactoryTaskGuidString;
    (*request.as_object())[std::string_view("displayName")] = "Daily run";
    {
        yyjson::value initial(Das::Utils::MakeYyjsonObject());
        (*initial.as_object())[std::string_view("retryCount")] = 5;
        (*request.as_object())[std::string_view("initialProperties")] =
            std::move(initial);
    }

    auto created = scheduler_->CreateRepositoryEntry(request);
    auto obj = created.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ((*obj)[std::string_view("entryId")].as_sint().value_or(-1), 0);
    EXPECT_EQ(
        (*obj)[std::string_view("displayName")].as_string().value_or(""),
        std::string_view("Daily run"));
    EXPECT_EQ(
        (*obj)[std::string_view("pluginGuid")].as_string().value_or(""),
        std::string_view(FactoryPluginGuidString));
    EXPECT_EQ(
        (*obj)[std::string_view("taskTypeGuid")].as_string().value_or(""),
        std::string_view(FactoryTaskGuidString));

    auto authoring = (*obj)[std::string_view("authoring")].as_object();
    ASSERT_TRUE(authoring.has_value());
    EXPECT_EQ(
        (*authoring)[std::string_view("revision")].as_sint().value_or(-1),
        0);

    auto accepted = (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(
        (*accepted)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("default"));
    EXPECT_EQ(
        (*accepted)[std::string_view("retryCount")].as_sint().value_or(-1),
        5);

    auto availability = (*obj)[std::string_view("availability")].as_object();
    ASSERT_TRUE(availability.has_value());
    EXPECT_EQ(
        (*availability)[std::string_view("state")].as_string().value_or(""),
        std::string_view("available"));

    auto persisted = settings_manager_->GetTaskRepositoryEntryJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    EXPECT_EQ(
        (*persisted_obj)[std::string_view("entryId")].as_sint().value_or(-1),
        0);
    auto persisted_props =
        (*persisted_obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(persisted_props.has_value());
    EXPECT_EQ(
        (*persisted_props)[std::string_view("retryCount")].as_sint().value_or(
            -1),
        5);

    auto scheduler_state = scheduler_->Get();
    EXPECT_EQ(
        (*scheduler_state.as_object())[std::string_view("tasks")]
            .as_array()
            ->size(),
        0u);

    auto scheduler_index = settings_manager_->GetSchedulerIndexJson("0");
    EXPECT_EQ(
        (*scheduler_index.as_object())[std::string_view("taskOrder")]
            .as_array()
            ->size(),
        0u);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryAuthoringGetReusesProviderSession)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Authoring repository entry", 8));
    auto created_obj = created.as_object();
    ASSERT_TRUE(created_obj.has_value());
    ASSERT_EQ(
        (*created_obj)[std::string_view("entryId")].as_sint().value_or(-1),
        0);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto                document_response =
        scheduler_->GetRepositoryEntryAuthoringDocument(0, request);
    auto response_obj = document_response.as_object();
    ASSERT_TRUE(response_obj.has_value());
    EXPECT_EQ(
        (*response_obj)[std::string_view("entryId")].as_sint().value_or(-1),
        0);
    EXPECT_EQ(
        (*response_obj)[std::string_view("revision")].as_sint().value_or(-1),
        0);
    auto document = (*response_obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    // Phase 84 strict wire replaces kind with viewKind; no Core-merged catalog.
    EXPECT_EQ(
        (*document)[std::string_view("viewKind")].as_string().value_or(""),
        std::string_view("formSequence"));
    EXPECT_FALSE(document->contains(std::string_view("catalog")));
    EXPECT_TRUE(response_obj->contains(std::string_view("diagnostics")));

    auto persisted = settings_manager_->GetTaskRepositoryEntryJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    auto persisted_authoring =
        (*persisted_obj)[std::string_view("authoring")].as_object();
    ASSERT_TRUE(persisted_authoring.has_value());
    EXPECT_EQ(
        (*persisted_authoring)[std::string_view("revision")].as_sint().value_or(
            -1),
        0);

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->authoring_session_count, 1);
    EXPECT_EQ(shared_state_->decoy_authoring_create_count, 0);
    EXPECT_EQ(shared_state_->get_document_count, 1);
    EXPECT_EQ(shared_state_->last_context_entry_id, 0);
    EXPECT_FALSE(shared_state_->last_context_had_task_id);
    EXPECT_EQ(shared_state_->last_context_task_id, -1);
    EXPECT_EQ(shared_state_->last_context_revision, 0);
    EXPECT_EQ(shared_state_->get_last_props_key1_value(), "default");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryAuthoringApplyPersistsAcceptedSettingsAndRevision)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Apply repository entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    auto change = MakeRepositoryAuthoringChange(0);
    auto result = scheduler_->ApplyRepositoryEntryAuthoringChange(0, change);
    auto result_obj = result.as_object();
    ASSERT_TRUE(result_obj.has_value());
    EXPECT_TRUE(
        (*result_obj)[std::string_view("ok")].as_bool().value_or(false));
    EXPECT_EQ(
        (*result_obj)[std::string_view("entryId")].as_sint().value_or(-1),
        0);
    auto accepted =
        (*result_obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(
        (*accepted)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("accepted"));
    auto document = (*result_obj)[std::string_view("document")].as_object();
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(
        (*document)[std::string_view("viewKind")].as_string().value_or(""),
        std::string_view("formSequence"));
    EXPECT_EQ(
        (*document)[std::string_view("revision")].as_sint().value_or(-1),
        1);
    EXPECT_TRUE(result_obj->contains(std::string_view("diagnostics")));

    auto persisted = settings_manager_->GetTaskRepositoryEntryJson("0", 0);
    auto persisted_obj = persisted.as_object();
    ASSERT_TRUE(persisted_obj.has_value());
    auto persisted_props =
        (*persisted_obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(persisted_props.has_value());
    EXPECT_EQ(
        (*persisted_props)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("accepted"));
    auto authoring =
        (*persisted_obj)[std::string_view("authoring")].as_object();
    ASSERT_TRUE(authoring.has_value());
    EXPECT_EQ(
        (*authoring)[std::string_view("revision")].as_sint().value_or(-1),
        1);
    EXPECT_EQ(
        (*authoring)[std::string_view("lastChangeKind")].as_string().value_or(
            ""),
        std::string_view("setValue"));
    EXPECT_FALSE(authoring->contains(std::string_view("kind")));
    EXPECT_TRUE(authoring->contains(std::string_view("sourceFingerprint")));
    EXPECT_FALSE(authoring->contains(std::string_view("migrationState")));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
    // Phase 84 strict Apply carries the document in the Apply result envelope,
    // so the Scheduler no longer falls back to a separate GetDocument call on
    // success.
    EXPECT_EQ(shared_state_->get_document_count, 0);
    EXPECT_EQ(shared_state_->last_context_entry_id, 0);
    EXPECT_FALSE(shared_state_->last_context_had_task_id);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryAuthoringApplyStaleBaseRevisionReturnsConflict)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Stale revision entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    auto persisted = settings_manager_->GetTaskRepositoryEntryJson("0", 0);
    auto authoring =
        (*persisted.as_object())[std::string_view("authoring")].as_object();
    ASSERT_TRUE(authoring.has_value());
    (*authoring)[std::string_view("revision")] = 3;
    ASSERT_EQ(
        settings_manager_->UpdateTaskRepositoryEntryJson("0", 0, persisted),
        DAS_S_OK);

    auto change = MakeRepositoryAuthoringChange(2);
    auto result = scheduler_->ApplyRepositoryEntryAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("revisionConflict"));
    EXPECT_EQ(
        (*obj)[std::string_view("currentRevision")].as_sint().value_or(-1),
        3);
    ExpectNoRepositoryAuthoringProviderCalls(shared_state_);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryAuthoringApplyMissingBaseRevisionReturnsConflict)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Missing base revision entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    auto change = MakeRepositoryAuthoringChange(std::nullopt);
    auto result = scheduler_->ApplyRepositoryEntryAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    // Phase 84 task/repository symmetry: the envelope validator runs before
    // the revision check. A missing baseRevision is reported as missingField,
    // not revisionConflict.
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("missingField"));
    ExpectNoRepositoryAuthoringProviderCalls(shared_state_);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryAuthoringApplyProviderFailureDoesNotPersist)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Provider failure entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->apply_ok = false;
    }

    auto change = MakeRepositoryAuthoringChange(0);
    auto result = scheduler_->ApplyRepositoryEntryAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("providerFailed"));

    auto persisted = settings_manager_->GetTaskRepositoryEntryJson("0", 0);
    auto authoring =
        (*persisted.as_object())[std::string_view("authoring")].as_object();
    ASSERT_TRUE(authoring.has_value());
    EXPECT_EQ(
        (*authoring)[std::string_view("revision")].as_sint().value_or(-1),
        0);
    auto persisted_props =
        (*persisted.as_object())[std::string_view("acceptedProperties")]
            .as_object();
    ASSERT_TRUE(persisted_props.has_value());
    EXPECT_EQ(
        (*persisted_props)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("default"));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryAuthoringApplyPersistenceFailureReturnsError)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Persistence failure entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    auto entry_path = settings_dir_ / DAS_PATH_LITERAL("0")
                      / DAS_PATH_LITERAL("taskRepository0.json");
    std::filesystem::remove(entry_path);
    std::filesystem::create_directory(entry_path);

    auto change = MakeRepositoryAuthoringChange(0);
    auto result = scheduler_->ApplyRepositoryEntryAuthoringChange(0, change);
    auto obj = result.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("persistenceFailed"));

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->apply_change_count, 1);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryListExcludesEntriesFromSchedulerLifecycle)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto first_request =
        MakeRepositoryCreateRequest("First repository entry", 1);
    auto first = scheduler_->CreateRepositoryEntry(first_request);
    auto first_obj = first.as_object();
    ASSERT_TRUE(first_obj.has_value());
    EXPECT_EQ(
        (*first_obj)[std::string_view("entryId")].as_sint().value_or(-1),
        0);

    auto second_request =
        MakeRepositoryCreateRequest("Second repository entry", 2);
    auto second = scheduler_->CreateRepositoryEntry(second_request);
    auto second_obj = second.as_object();
    ASSERT_TRUE(second_obj.has_value());
    EXPECT_EQ(
        (*second_obj)[std::string_view("entryId")].as_sint().value_or(-1),
        1);

    auto repository = scheduler_->GetTaskRepository();
    auto repository_obj = repository.as_object();
    ASSERT_TRUE(repository_obj.has_value());
    auto entries = (*repository_obj)[std::string_view("entries")].as_array();
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 2u);
    EXPECT_EQ(
        (*(*entries)[0].as_object())[std::string_view("entryId")]
            .as_sint()
            .value_or(-1),
        0);
    EXPECT_EQ(
        (*(*entries)[1].as_object())[std::string_view("entryId")]
            .as_sint()
            .value_or(-1),
        1);

    auto scheduler_state = scheduler_->Get();
    auto scheduler_obj = scheduler_state.as_object();
    ASSERT_TRUE(scheduler_obj.has_value());
    auto tasks = (*scheduler_obj)[std::string_view("tasks")].as_array();
    ASSERT_TRUE(tasks.has_value());
    EXPECT_EQ(tasks->size(), 0u);

    auto scheduler_index = settings_manager_->GetSchedulerIndexJson("0");
    auto task_order =
        (*scheduler_index.as_object())[std::string_view("taskOrder")]
            .as_array();
    ASSERT_TRUE(task_order.has_value());
    EXPECT_EQ(task_order->size(), 0u);

    EXPECT_EQ(scheduler_->DeleteTask(0), DAS_E_NOT_FOUND);
    auto still_persisted =
        settings_manager_->GetTaskRepositoryEntryJson("0", 0);
    auto still_persisted_obj = still_persisted.as_object();
    ASSERT_TRUE(still_persisted_obj.has_value());
    EXPECT_EQ(
        (*still_persisted_obj)[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    EXPECT_EQ(scheduler_->Enable(), DAS_E_OBJECT_NOT_INIT);
    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_TRUE((shared_state_->executed_count == 0));
    EXPECT_EQ(shared_state_->compile_count, 0);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryDeleteRemovesOnlyRepositoryEntry)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto first = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("First repository entry", 1));
    ASSERT_EQ(
        (*first.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);
    auto second = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Second repository entry", 2));
    ASSERT_EQ(
        (*second.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        1);

    EXPECT_EQ(scheduler_->DeleteRepositoryEntry(0), DAS_S_OK);

    EXPECT_FALSE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskRepository0.json")));
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskRepository1.json")));

    auto repository = scheduler_->GetTaskRepository();
    auto entries =
        (*repository.as_object())[std::string_view("entries")].as_array();
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 1u);
    EXPECT_EQ(
        (*(*entries)[0].as_object())[std::string_view("entryId")]
            .as_sint()
            .value_or(-1),
        1);

    auto scheduler_state = scheduler_->Get();
    EXPECT_EQ(
        (*scheduler_state.as_object())[std::string_view("tasks")]
            .as_array()
            ->size(),
        0u);
    auto scheduler_index = settings_manager_->GetSchedulerIndexJson("0");
    EXPECT_EQ(
        (*scheduler_index.as_object())[std::string_view("taskOrder")]
            .as_array()
            ->size(),
        0u);
}

TEST_F(SchedulerRuntimeBackedTest, SchedulerRepositoryDeleteMissingEntry)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    EXPECT_EQ(scheduler_->DeleteRepositoryEntry(404), DAS_E_NOT_FOUND);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryRenamePreservesAuthoringMetadataAndAllowsDuplicates)
{
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto first = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Original A", 1));
    ASSERT_EQ(
        (*first.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);
    auto second = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Original B", 2));
    ASSERT_EQ(
        (*second.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        1);

    auto persisted = settings_manager_->GetTaskRepositoryEntryJson("0", 0);
    auto authoring =
        (*persisted.as_object())[std::string_view("authoring")].as_object();
    ASSERT_TRUE(authoring.has_value());
    (*authoring)[std::string_view("revision")] = 7;
    (*authoring)[std::string_view("sourceFingerprint")] = "source-abc";
    ASSERT_EQ(
        settings_manager_->UpdateTaskRepositoryEntryJson("0", 0, persisted),
        DAS_S_OK);

    auto rename_request = MakeRepositoryRenameRequest("Duplicate name");
    auto renamed = scheduler_->RenameRepositoryEntry(0, rename_request);
    auto renamed_obj = renamed.as_object();
    ASSERT_TRUE(renamed_obj.has_value());
    EXPECT_EQ(
        (*renamed_obj)[std::string_view("displayName")].as_string().value_or(
            ""),
        std::string_view("Duplicate name"));
    auto renamed_authoring =
        (*renamed_obj)[std::string_view("authoring")].as_object();
    ASSERT_TRUE(renamed_authoring.has_value());
    EXPECT_EQ(
        (*renamed_authoring)[std::string_view("revision")].as_sint().value_or(
            -1),
        7);
    EXPECT_EQ(
        (*renamed_authoring)[std::string_view("sourceFingerprint")]
            .as_string()
            .value_or(""),
        std::string_view("source-abc"));

    auto duplicate = scheduler_->RenameRepositoryEntry(1, rename_request);
    auto duplicate_obj = duplicate.as_object();
    ASSERT_TRUE(duplicate_obj.has_value());
    EXPECT_EQ(
        (*duplicate_obj)[std::string_view("displayName")].as_string().value_or(
            ""),
        std::string_view("Duplicate name"));

    auto repository = scheduler_->GetTaskRepository();
    auto entries =
        (*repository.as_object())[std::string_view("entries")].as_array();
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 2u);
    EXPECT_EQ(
        (*(*entries)[0].as_object())[std::string_view("displayName")]
            .as_string()
            .value_or(""),
        std::string_view("Duplicate name"));
    EXPECT_EQ(
        (*(*entries)[1].as_object())[std::string_view("displayName")]
            .as_string()
            .value_or(""),
        std::string_view("Duplicate name"));
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableMissingPluginReason)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        MissingPluginGuidString,
        FactoryTaskGuidString,
        "Missing plugin entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto entry = SingleRepositoryEntry(scheduler_->GetTaskRepository());
    ExpectRepositoryAvailability(entry, "pluginUnavailable");
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskRepository0.json")));
    ExpectNoRepositoryAuthoringProviderCalls(shared_state_);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableDisabledPluginReason)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        "Disabled plugin entry");

    ASSERT_EQ(
        scheduler_->Initialize(plugin_dir_, {FactoryPluginGuid}),
        DAS_S_OK);

    auto entry = SingleRepositoryEntry(scheduler_->GetTaskRepository());
    ExpectRepositoryAvailability(entry, "pluginUnavailable");
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskRepository0.json")));
    ExpectNoRepositoryAuthoringProviderCalls(shared_state_);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableBannedPluginReason)
{
    auto banned_dir = plugin_dir_ / DAS_PATH_LITERAL("BannedPlugin");
    std::filesystem::create_directories(banned_dir);
    WriteFactoryPluginManifest(
        banned_dir / DAS_PATH_LITERAL("BannedPlugin.json"),
        BannedPluginGuidString);
    {
        std::ofstream marker(
            banned_dir / DAS_PATH_LITERAL("BannedPlugin.willBeDelete"),
            std::ios::trunc);
        marker << "pending deletion";
    }
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        BannedPluginGuidString,
        FactoryTaskGuidString,
        "Banned plugin entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto entry = SingleRepositoryEntry(scheduler_->GetTaskRepository());
    ExpectRepositoryAvailability(entry, "pluginUnavailable");
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskRepository0.json")));
    ExpectNoRepositoryAuthoringProviderCalls(shared_state_);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableLoadFailedReason)
{
    WriteFactoryPluginManifest(
        plugin_dir_ / DAS_PATH_LITERAL("LoadFailedPlugin.json"),
        LoadFailedPluginGuidString,
        FactoryTaskGuidString,
        true,
        "CSharp");
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        LoadFailedPluginGuidString,
        FactoryTaskGuidString,
        "Load failed entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto entry = SingleRepositoryEntry(scheduler_->GetTaskRepository());
    ExpectRepositoryAvailability(entry, "pluginLoadFailed");
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskRepository0.json")));
    ExpectNoRepositoryAuthoringProviderCalls(shared_state_);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableMissingTaskTypeReason)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        FactoryPluginGuidString,
        MissingTaskTypeGuidString,
        "Missing task type entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto entry = SingleRepositoryEntry(scheduler_->GetTaskRepository());
    ExpectRepositoryAvailability(entry, "taskTypeUnavailable");
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskRepository0.json")));
    ExpectNoRepositoryAuthoringProviderCalls(shared_state_);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableAuthoringCapabilityMissingReason)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        false);
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        "No authoring capability entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto entry = SingleRepositoryEntry(scheduler_->GetTaskRepository());
    ExpectRepositoryAvailability(entry, "capabilityMissing");
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskRepository0.json")));
    ExpectNoRepositoryAuthoringProviderCalls(shared_state_);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableNoProviderCallForRepositoryGet)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        MissingPluginGuidString,
        FactoryTaskGuidString,
        "No provider call entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    (void)scheduler_->GetTaskRepository();

    ExpectNoRepositoryAuthoringProviderCalls(shared_state_);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableMissingPluginAuthoringNoProviderCall)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        MissingPluginGuidString,
        FactoryTaskGuidString,
        "Missing plugin authoring entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectRepositoryAuthoringRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "pluginUnavailable");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableDisabledAuthoringNoProviderCall)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        "Disabled plugin authoring entry");

    ASSERT_EQ(
        scheduler_->Initialize(plugin_dir_, {FactoryPluginGuid}),
        DAS_S_OK);

    ExpectRepositoryAuthoringRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "pluginUnavailable");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryBannedPluginAuthoringNoProviderCall)
{
    auto banned_dir = plugin_dir_ / DAS_PATH_LITERAL("BannedPlugin");
    std::filesystem::create_directories(banned_dir);
    WriteFactoryPluginManifest(
        banned_dir / DAS_PATH_LITERAL("BannedPlugin.json"),
        BannedPluginGuidString);
    {
        std::ofstream marker(
            banned_dir / DAS_PATH_LITERAL("BannedPlugin.willBeDelete"),
            std::ios::trunc);
        marker << "pending deletion";
    }
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        BannedPluginGuidString,
        FactoryTaskGuidString,
        "Banned plugin authoring entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectRepositoryAuthoringRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "pluginUnavailable");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryLoadFailedAuthoringNoProviderCall)
{
    WriteFactoryPluginManifest(
        plugin_dir_ / DAS_PATH_LITERAL("LoadFailedPlugin.json"),
        LoadFailedPluginGuidString,
        FactoryTaskGuidString,
        true,
        "CSharp");
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        LoadFailedPluginGuidString,
        FactoryTaskGuidString,
        "Load failed authoring entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectRepositoryAuthoringRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "pluginLoadFailed");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableMissingTaskTypeAuthoringNoProviderCall)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        FactoryPluginGuidString,
        MissingTaskTypeGuidString,
        "Missing task type authoring entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectRepositoryAuthoringRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "taskTypeUnavailable");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableAuthoringCapabilityMissingAuthoringNoProviderCall)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        false);
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        "No authoring capability authoring entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectRepositoryAuthoringRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "capabilityMissing");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableMissingPluginCompileNoProviderCall)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        MissingPluginGuidString,
        FactoryTaskGuidString,
        "Missing plugin compile entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectRepositoryCompileRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "pluginUnavailable");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableDisabledCompileNoProviderCall)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        "Disabled plugin compile entry");

    ASSERT_EQ(
        scheduler_->Initialize(plugin_dir_, {FactoryPluginGuid}),
        DAS_S_OK);

    ExpectRepositoryCompileRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "pluginUnavailable");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryBannedPluginCompileNoProviderCall)
{
    auto banned_dir = plugin_dir_ / DAS_PATH_LITERAL("BannedPlugin");
    std::filesystem::create_directories(banned_dir);
    WriteFactoryPluginManifest(
        banned_dir / DAS_PATH_LITERAL("BannedPlugin.json"),
        BannedPluginGuidString);
    {
        std::ofstream marker(
            banned_dir / DAS_PATH_LITERAL("BannedPlugin.willBeDelete"),
            std::ios::trunc);
        marker << "pending deletion";
    }
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        BannedPluginGuidString,
        FactoryTaskGuidString,
        "Banned plugin compile entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectRepositoryCompileRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "pluginUnavailable");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryLoadFailedCompileNoProviderCall)
{
    WriteFactoryPluginManifest(
        plugin_dir_ / DAS_PATH_LITERAL("LoadFailedPlugin.json"),
        LoadFailedPluginGuidString,
        FactoryTaskGuidString,
        true,
        "CSharp");
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        LoadFailedPluginGuidString,
        FactoryTaskGuidString,
        "Load failed compile entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectRepositoryCompileRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "pluginLoadFailed");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableMissingTaskTypeCompileNoProviderCall)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        FactoryPluginGuidString,
        MissingTaskTypeGuidString,
        "Missing task type compile entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectRepositoryCompileRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "taskTypeUnavailable");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableAuthoringCapabilityMissingCompileNoProviderCall)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        false);
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        "No authoring capability compile entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectRepositoryCompileRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "capabilityMissing");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryRunningAuthoringGetApplyNoProviderCall)
{
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] = FactoryTaskGuidString;
    (*task0.as_object())[std::string_view("pluginGuid")] =
        FactoryPluginGuidString;
    (*task0.as_object())[std::string_view("nextExecutionTime")] = 4070908800;
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Running authoring entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    ExpectRepositoryAuthoringRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "taskWorking");

    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
    ASSERT_TRUE(
        WaitForSchedulerStopped(*scheduler_, std::chrono::milliseconds(2000)));
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryRunningCompileNoProviderCall)
{
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] = FactoryTaskGuidString;
    (*task0.as_object())[std::string_view("pluginGuid")] =
        FactoryPluginGuidString;
    (*task0.as_object())[std::string_view("nextExecutionTime")] = 4070908800;
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Running compile entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    ExpectRepositoryCompileRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "taskWorking");

    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
    ASSERT_TRUE(
        WaitForSchedulerStopped(*scheduler_, std::chrono::milliseconds(2000)));
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryStoppingAuthoringGetApplyNoProviderCall)
{
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] = FactoryTaskGuidString;
    (*task0.as_object())[std::string_view("pluginGuid")] =
        FactoryPluginGuidString;
    (*task0.as_object())[std::string_view("nextExecutionTime")] =
        yyjson::value{};
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Stopping authoring entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->block_do = true;
    }
    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    {
        ipc_scoped_lock lock(shared_state_->mutex);
        ASSERT_TRUE(shared_state_->cv.wait_for(
            lock,
            std::chrono::seconds(2),
            [this] { return shared_state_->do_entered; }));
    }

    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
    ASSERT_EQ(scheduler_->Status(), SchedulerState::Stopping);
    ExpectRepositoryAuthoringRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "taskWorking");

    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->unblock_do = true;
    }
    shared_state_->cv.notify_all();
    ASSERT_TRUE(
        WaitForSchedulerStopped(*scheduler_, std::chrono::milliseconds(2000)));
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryStoppingCompileNoProviderCall)
{
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] = FactoryTaskGuidString;
    (*task0.as_object())[std::string_view("pluginGuid")] =
        FactoryPluginGuidString;
    (*task0.as_object())[std::string_view("nextExecutionTime")] =
        yyjson::value{};
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    auto created = scheduler_->CreateRepositoryEntry(
        MakeRepositoryCreateRequest("Stopping compile entry", 8));
    ASSERT_EQ(
        (*created.as_object())[std::string_view("entryId")].as_sint().value_or(
            -1),
        0);

    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->block_do = true;
    }
    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    {
        ipc_scoped_lock lock(shared_state_->mutex);
        ASSERT_TRUE(shared_state_->cv.wait_for(
            lock,
            std::chrono::seconds(2),
            [this] { return shared_state_->do_entered; }));
    }

    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
    ASSERT_EQ(scheduler_->Status(), SchedulerState::Stopping);
    ExpectRepositoryCompileRejectedWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        "taskWorking");

    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->unblock_do = true;
    }
    shared_state_->cv.notify_all();
    ASSERT_TRUE(
        WaitForSchedulerStopped(*scheduler_, std::chrono::milliseconds(2000)));
}

// ---------------------------------------------------------------------------
// RunTaskNow（立即执行）
// ---------------------------------------------------------------------------

namespace
{
    yyjson::value MakeFutureScheduledTask(int64_t task_id, bool enabled = true)
    {
        yyjson::value task(Das::Utils::MakeYyjsonObject());
        (*task.as_object())[std::string_view("id")] = task_id;
        (*task.as_object())[std::string_view("taskGuid")] =
            FactoryTaskGuidString;
        (*task.as_object())[std::string_view("pluginGuid")] =
            FactoryPluginGuidString;
        // 远期时间：调度循环不会触发，只有 RunTaskNow 置 due 才执行。
        (*task.as_object())[std::string_view("nextExecutionTime")] = 4070908800;
        (*task.as_object())[std::string_view("enabled")] = enabled;
        (*task.as_object())[std::string_view("properties")] =
            yyjson::value(Das::Utils::MakeYyjsonObject());
        return task;
    }

    yyjson::value MakeDueScheduledTask(int64_t task_id, bool enabled = true)
    {
        yyjson::value task = MakeFutureScheduledTask(task_id, enabled);
        // 过去时间：驱动 Stopped 排空链在当前 tick 结束后重投下一个。
        (*task.as_object())[std::string_view("nextExecutionTime")] = 1;
        return task;
    }

    // 往 BusinessThread 投递的 Disable 调用载体：回调体内直接调 Disable，
    // 复现生产 stop 路径的线程语境（Router 把 handler 派发到 BT 上执行）。
    class BusinessThreadDisableCallback final : public IDasAsyncCallback
    {
    public:
        BusinessThreadDisableCallback(
            SchedulerService&       scheduler,
            std::promise<DasResult> disable_result)
            : scheduler_(scheduler), disable_result_(std::move(disable_result))
        {
        }

        uint32_t AddRef() override { return ++ref_count_; }

        uint32_t Release() override
        {
            auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult QueryInterface(const DasGuid& iid, void** pp) override
        {
            if (pp == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasAsyncCallback>())
            {
                *pp = this;
                AddRef();
                return DAS_S_OK;
            }
            *pp = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult Do() noexcept override
        {
            try
            {
                disable_result_.set_value(scheduler_.Disable());
            }
            catch (...)
            {
                try
                {
                    disable_result_.set_exception(std::current_exception());
                }
                catch (...)
                {
                }
            }
            return DAS_S_OK;
        }

    private:
        SchedulerService&       scheduler_;
        std::promise<DasResult> disable_result_;
        std::atomic<uint32_t>   ref_count_{1};
    };

    // 占位回调：把 BusinessThread 挡在 Do 内直到主线程放行（带超时防
    // 挂死），用于把 ExecuteTaskOnce 的落地时机钉死在 Enable 之后——
    // 状态互斥复检的确定性构造。
    class BusinessThreadBarrierCallback final : public IDasAsyncCallback
    {
    public:
        explicit BusinessThreadBarrierCallback(
            std::shared_ptr<std::promise<void>> release)
            : release_(std::move(release))
        {
        }

        uint32_t AddRef() override { return ++ref_count_; }

        uint32_t Release() override
        {
            auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult QueryInterface(const DasGuid& iid, void** pp) override
        {
            if (pp == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasAsyncCallback>())
            {
                *pp = this;
                AddRef();
                return DAS_S_OK;
            }
            *pp = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult Do() noexcept override
        {
            release_->get_future().wait_for(std::chrono::seconds(2));
            return DAS_S_OK;
        }

    private:
        std::shared_ptr<std::promise<void>> release_;
        std::atomic<uint32_t>               ref_count_{1};
    };

    ::testing::AssertionResult WaitForExecutedCount(
        FactoryTaskSharedState&   state,
        size_t                    expected,
        std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                const ipc_scoped_lock lock(state.mutex);
                if (state.executed_count >= expected)
                {
                    return ::testing::AssertionSuccess();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return ::testing::AssertionFailure()
               << "executed_count did not reach " << expected << " within "
               << timeout.count() << "ms";
    }
} // namespace

TEST_F(
    SchedulerRuntimeBackedTest,
    RunNow_ExecutesTaskInStoppedState_AndBlocksMutations)
{
    WriteSchedulerState(
        *settings_manager_,
        1,
        {0},
        {MakeFutureScheduledTask(0)});
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    ASSERT_EQ(scheduler_->Status(), SchedulerState::Stopped);

    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->block_do = true;
    }
    ASSERT_EQ(scheduler_->RunTaskNow(0), DAS_S_OK);
    {
        ipc_scoped_lock lock(shared_state_->mutex);
        ASSERT_TRUE(shared_state_->cv.wait_for(
            lock,
            std::chrono::seconds(2),
            [this] { return shared_state_->do_entered; }));
    }

    // 执行中：状态保持 Stopped，Get() 暴露 runningTaskId，
    // Enable/二次 RunNow/DeleteTask 全部被 busy 拒绝（卡住语义）。
    ASSERT_EQ(scheduler_->Status(), SchedulerState::Stopped);
    {
        auto state_json = scheduler_->Get();
        auto obj = state_json.as_object();
        ASSERT_TRUE(obj);
        auto running = obj->operator[](std::string_view("runningTaskId"));
        ASSERT_TRUE(running.as_sint().has_value());
        ASSERT_EQ(running.as_sint().value(), 0);
    }
    ASSERT_EQ(scheduler_->Enable(), DAS_E_TASK_WORKING);
    ASSERT_EQ(scheduler_->RunTaskNow(0), DAS_E_TASK_WORKING);
    ASSERT_EQ(scheduler_->DeleteTask(0), DAS_E_TASK_WORKING);

    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->unblock_do = true;
    }
    shared_state_->cv.notify_all();
    ASSERT_TRUE(WaitForExecutedCount(
        *shared_state_,
        1,
        std::chrono::milliseconds(2000)));

    // executed_count 在 Do 入口自增，槽位清零在 OnTick 收尾——轮询等
    // Get() 的 runningTaskId 消失（即收尾完成）。
    bool running_cleared = false;
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto state_json = scheduler_->Get();
            auto obj = state_json.as_object();
            if (obj && !obj->contains(std::string_view("runningTaskId")))
            {
                running_cleared = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ASSERT_TRUE(running_cleared);

    // 执行完成：状态仍为 Stopped，nextExecutionTime 被插件返回值
    // （2030-01-01）重算。
    ASSERT_EQ(scheduler_->Status(), SchedulerState::Stopped);
    {
        auto state_json = scheduler_->Get();
        auto obj = state_json.as_object();
        ASSERT_TRUE(obj);

        auto tasks = obj->operator[](std::string_view("tasks")).as_array();
        ASSERT_TRUE(tasks);
        ASSERT_EQ(tasks->size(), 1U);
        auto next_time = (*tasks)[0]
                             .as_object()
                             .value()
                             .operator[](std::string_view("nextExecutionTime"))
                             .as_sint();
        ASSERT_TRUE(next_time.has_value());
        // FactoryBackedTask::GetNextExecutionTime 返回 2030-01-01 00:00:SS
        // （SS=instance_id）：确认被插件值重算，而非残留置 due 用的远期值。
        ASSERT_GE(next_time.value(), 1893456000);
        ASSERT_LT(next_time.value(), 1893456100);
    }
}

TEST_F(
    SchedulerRuntimeBackedTest,
    DisableFromBusinessThread_PumpsAndDrainsPendingTicks)
{
    // 镜像生产交错：stop 调用（BT 上）排在长 Do 执行中的 tick 之后、
    // 排空重投的下一个 tick 之前——Disable 的排空等待必须泵化（BT 上
    // sync_wait 自等死锁），泵内把排空链上的 tick 跑完才返回。
    WriteSchedulerState(
        *settings_manager_,
        2,
        {0, 1},
        {MakeFutureScheduledTask(0), MakeDueScheduledTask(1)});
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    ASSERT_EQ(scheduler_->Status(), SchedulerState::Stopped);

    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->block_do = true;
    }
    ASSERT_EQ(scheduler_->RunTaskNow(0), DAS_S_OK);
    {
        ipc_scoped_lock lock(shared_state_->mutex);
        ASSERT_TRUE(shared_state_->cv.wait_for(
            lock,
            std::chrono::seconds(2),
            [this] { return shared_state_->do_entered; }));
    }

    // Do 阻塞期间投 stop：disable 回调入 BT 队列，排在当前 tick 之后、
    // 排空重投的 tick 之前。
    std::promise<DasResult> disable_result_promise;
    auto                    disable_done = disable_result_promise.get_future();
    {
        // 智能指针优先：构造时那份引用由 DasPtr 接管（Attach 不叠加
        // AddRef），PostToBusinessThread 自建引用，作用域析构成对归还。
        const DasPtr<IDasAsyncCallback> callback(
            DasPtr<IDasAsyncCallback>::Attach(
                static_cast<IDasAsyncCallback*>(
                    new BusinessThreadDisableCallback(
                        *scheduler_,
                        std::move(disable_result_promise)))));
        ipc_sp_->PostToBusinessThread(callback.Get());
    }

    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->unblock_do = true;
    }
    shared_state_->cv.notify_all();

    // 卡死防线：泵化排空若退化为自等，5s 超时处失败而不是挂死进程。
    ASSERT_EQ(
        disable_done.wait_for(std::chrono::seconds(5)),
        std::future_status::ready);
    static_cast<void>(disable_done.get());

    // 断言锚定析构之后：persist 队列 drain-then-exit 的 join 是确定性
    // 观测点。
    scheduler_.reset();

    for (const int64_t task_id :
         {static_cast<int64_t>(0), static_cast<int64_t>(1)})
    {
        auto task_json = settings_manager_->GetTaskInstanceJson("0", task_id);
        EXPECT_FALSE(
            (*task_json.as_object())[std::string_view("nextExecutionTime")]
                .is_null());
    }
}

TEST_F(SchedulerRuntimeBackedTest, RunNow_RejectedWhenRunning)
{
    WriteSchedulerState(
        *settings_manager_,
        1,
        {0},
        {MakeFutureScheduledTask(0)});
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    // 调度器运行中：立即执行被拒（状态隔离，想手动跑先停调度器）。
    ASSERT_EQ(scheduler_->RunTaskNow(0), DAS_E_TASK_WORKING);

    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
    ASSERT_TRUE(
        WaitForSchedulerStopped(*scheduler_, std::chrono::milliseconds(2000)));
}

TEST_F(SchedulerRuntimeBackedTest, RunNow_RejectsUnknownAndDisabledTask)
{
    WriteSchedulerState(
        *settings_manager_,
        2,
        {0, 1},
        {MakeFutureScheduledTask(0), MakeFutureScheduledTask(1, false)});
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ASSERT_EQ(scheduler_->RunTaskNow(999), DAS_E_NOT_FOUND);
    ASSERT_EQ(scheduler_->RunTaskNow(1), DAS_E_FAIL);
}

TEST_F(
    SchedulerRuntimeBackedTest,
    RunNow_RunsOnlyRequestedTask_SkipsOverdueTasks)
{
    // 生产复刻：任务 1 是历史残留 overdue（nextExecutionTime=1）且
    // enabled。用户只点名运行任务 0——手动执行链不得连带清算任务 1
    // （旧全局选任务语义下 due 更早的 overdue 任务会插到点名任务前
    // 被执行，随后排空链拉起点名任务）。
    WriteSchedulerState(
        *settings_manager_,
        2,
        {0, 1},
        {MakeFutureScheduledTask(0), MakeDueScheduledTask(1)});
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    ASSERT_EQ(scheduler_->Status(), SchedulerState::Stopped);

    ASSERT_EQ(scheduler_->RunTaskNow(0), DAS_S_OK);
    ASSERT_TRUE(WaitForExecutions(1, std::chrono::milliseconds(2000)));

    // 点名任务执行完成且排空判定点已过：overdue 任务未被拉起。宽限
    // 窗远大于一次 tick 重投链；若 bug 存在，executed_count 会立刻变 2。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->executed_count, 1u);
    }

    // 任务 1 未被消费：持久化 nextExecutionTime 保持原始 overdue 值。
    auto overdue_json = settings_manager_->GetTaskInstanceJson("0", 1);
    EXPECT_EQ(
        (*overdue_json.as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value_or(0),
        1);

    scheduler_.reset();
}

TEST_F(
    SchedulerRuntimeBackedTest,
    RunNow_SequentialRequests_EachTaskRunsOnDemand)
{
    // 两次点名各自生效：第二次手动执行不因首次执行链终止而丢失。
    WriteSchedulerState(
        *settings_manager_,
        2,
        {0, 1},
        {MakeFutureScheduledTask(0), MakeFutureScheduledTask(1)});
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    ASSERT_EQ(scheduler_->Status(), SchedulerState::Stopped);

    ASSERT_EQ(scheduler_->RunTaskNow(0), DAS_S_OK);
    ASSERT_TRUE(WaitForExecutions(1, std::chrono::milliseconds(2000)));

    // 第二次点名可能撞上首次执行的收尾段（执行槽未清零被 busy 拒），
    // 带界轮询直到接受。
    DasResult  run_second = DAS_E_TASK_WORKING;
    const auto second_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < second_deadline)
    {
        run_second = scheduler_->RunTaskNow(1);
        if (run_second == DAS_S_OK)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(run_second, DAS_S_OK);
    ASSERT_TRUE(WaitForExecutions(2, std::chrono::milliseconds(2000)));

    scheduler_.reset();
}

TEST_F(SchedulerRuntimeBackedTest, RunNow_DroppedWhenEnabledBeforeLanding)
{
    // 状态互斥：投递后落地前调度器被 Enable 抢先转 Running，手动执行
    // 必须作废——调度 tick 与手动单任务两条通路绝不并发执行。
    WriteSchedulerState(
        *settings_manager_,
        1,
        {0},
        {MakeFutureScheduledTask(0)});
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // 先占住 BusinessThread：随后投递的 ExecuteTaskOnce 排在占位之后，
    // 落地时机由主线程的 release 控制。
    auto release = std::make_shared<std::promise<void>>();
    {
        const DasPtr<IDasAsyncCallback> barrier(
            DasPtr<IDasAsyncCallback>::Attach(
                static_cast<IDasAsyncCallback*>(
                    new BusinessThreadBarrierCallback(release))));
        ipc_sp_->PostToBusinessThread(barrier.Get());
    }

    // 手动执行在队列中排队（落地被占位挡住），随即 Enable 抢先转
    // Running（执行槽空闲，Enable 不被 busy 拒）。
    ASSERT_EQ(scheduler_->RunTaskNow(0), DAS_S_OK);
    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);

    release->set_value();

    // 宽限窗：若落地时缺少状态复检，ExecuteTaskOnce 会在 Running 态
    // 插队执行任务（executed_count 变 1）。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        EXPECT_EQ(shared_state_->executed_count, 0u);
    }

    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
    ASSERT_TRUE(
        WaitForSchedulerStopped(*scheduler_, std::chrono::milliseconds(2000)));
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableMissingPluginDeleteRenameNoProviderCall)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        MissingPluginGuidString,
        FactoryTaskGuidString,
        "Missing plugin entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectUnavailableRenameDeleteWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        settings_dir_,
        "pluginUnavailable");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableDisabledDeleteRenameNoProviderCall)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        "Disabled plugin entry");

    ASSERT_EQ(
        scheduler_->Initialize(plugin_dir_, {FactoryPluginGuid}),
        DAS_S_OK);

    ExpectUnavailableRenameDeleteWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        settings_dir_,
        "pluginUnavailable");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableBannedDeleteRenameNoProviderCall)
{
    auto banned_dir = plugin_dir_ / DAS_PATH_LITERAL("BannedPlugin");
    std::filesystem::create_directories(banned_dir);
    WriteFactoryPluginManifest(
        banned_dir / DAS_PATH_LITERAL("BannedPlugin.json"),
        BannedPluginGuidString);
    {
        std::ofstream marker(
            banned_dir / DAS_PATH_LITERAL("BannedPlugin.willBeDelete"),
            std::ios::trunc);
        marker << "pending deletion";
    }
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        BannedPluginGuidString,
        FactoryTaskGuidString,
        "Banned plugin entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectUnavailableRenameDeleteWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        settings_dir_,
        "pluginUnavailable");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableLoadFailedDeleteRenameNoProviderCall)
{
    WriteFactoryPluginManifest(
        plugin_dir_ / DAS_PATH_LITERAL("LoadFailedPlugin.json"),
        LoadFailedPluginGuidString,
        FactoryTaskGuidString,
        true,
        "CSharp");
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        LoadFailedPluginGuidString,
        FactoryTaskGuidString,
        "Load failed entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectUnavailableRenameDeleteWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        settings_dir_,
        "pluginLoadFailed");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableMissingTaskTypeDeleteRenameNoProviderCall)
{
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        FactoryPluginGuidString,
        MissingTaskTypeGuidString,
        "Missing task type entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectUnavailableRenameDeleteWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        settings_dir_,
        "taskTypeUnavailable");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerRepositoryUnavailableAuthoringCapabilityMissingDeleteRenameNoProviderCall)
{
    WriteFactoryPluginManifest(
        manifest_path_,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        false);
    SeedRepositoryEntry(
        *settings_manager_,
        0,
        FactoryPluginGuidString,
        FactoryTaskGuidString,
        "No authoring capability entry");

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    ExpectUnavailableRenameDeleteWithoutProviderCalls(
        *scheduler_,
        shared_state_,
        settings_dir_,
        "capabilityMissing");
}

TEST_F(
    SchedulerRuntimeBackedTest,
    SchedulerExecutionAuthoringCompileFailureSkipsDo)
{
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] = FactoryTaskGuidString;
    (*task0.as_object())[std::string_view("pluginGuid")] =
        FactoryPluginGuidString;
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    {
        const ipc_scoped_lock lock(shared_state_->mutex);
        shared_state_->compile_ok = false;
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);
    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);

    const ipc_scoped_lock lock(shared_state_->mutex);
    EXPECT_EQ(shared_state_->compile_count, 1);
    EXPECT_EQ(shared_state_->get_last_compile_purpose(), "execution");
    EXPECT_TRUE((shared_state_->executed_count == 0));
}

// ============================================================
// Scheduler controller validation tests
// ============================================================

namespace
{

    // 控制器直连具体 SchedulerService（接口面退役）：fake 接口替身随之退
    // 役——控制器用例改跑真实调度器（轻量装配见 SchedulerControllerTest
    // 夹具），断言聚焦控制器翻译面（guard 错误封套/成功封套形状/errorKind
    // 透传）；服务负载语义由本文件其余真实链用例背书。

    /// Build a Beast::HttpRequest with path parameters set.
    Das::Http::Beast::HttpRequest MakeRequest(
        const std::string&                   target,
        const std::string&                   body,
        std::map<std::string, std::string>&& path_params)
    {
        namespace http = boost::beast::http;
        http::request<http::string_body> raw_req{http::verb::post, target, 11};
        raw_req.body() = body;
        raw_req.prepare_payload();

        Das::Http::Beast::HttpRequest req{std::move(raw_req)};
        for (auto& [k, v] : path_params)
        {
            req.SetPathParameter(k, v);
        }
        return req;
    }

    yyjson::value ReleaseJson(Das::Http::Beast::HttpResponse&& response)
    {
        return *Das::Utils::ParseYyjsonFromString(response.Release().body());
    }

} // namespace

class SchedulerControllerTest : public ::testing::Test
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

        settings_manager_ =
            std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContext(false);
        plugin_manager_ = std::make_unique<
            Das::Core::ForeignInterfaceHost::TestablePluginManager>(
            *settings_manager_,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_.Get()));
        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_.Get()));
        controller_ = std::make_unique<Das::Http::DasSchedulerController>(
            *scheduler_,
            plugin_dir_);

        // IO 线程：Start 用例 Enable 排 tick timer / BT 投递需要 IPC 泵
        // （SchedulerExecutionTest 同款轻量装配——work guard 裸 io.run，
        // 无远端 peer，不驱动 IPC 接收循环）。
        io_thread_ = std::thread(
            [this]()
            {
                auto& io = ipc_sp_->GetIoContext();
                const boost::asio::executor_work_guard<
                    boost::asio::io_context::executor_type>
                    work(io.get_executor());
                io.run();
            });
    }

    void TearDown() override
    {
        ipc_sp_->GetIoContext().stop();
        if (io_thread_.joinable())
        {
            io_thread_.join();
        }
        controller_.reset();
        scheduler_.reset();
        plugin_manager_.reset();
        settings_manager_.reset();
        std::filesystem::remove_all(test_dir_);
    }

    // 注册 FakeTask 特征 + 种入任务实例 0 + Initialize——Start/Stop/任务
    // 变更/仓库用例需要「已初始化且含可用实例」的真实调度器。
    FakeTask* SeedInitializedWithTask()
    {
        return SetupSchedulerWithFakeTask(
            *scheduler_,
            *settings_manager_,
            plugin_dir_,
            *plugin_manager_);
    }

    // 直连经真实调度器建一个仓库条目，返回分配的 entryId。不带
    // initialProperties——FakeTask 无 descriptor 清单，非空初始属性会被
    // 真实校验拒绝（与控制器 create 用例同款最小请求形状）。
    int64_t SeedRepositoryEntry(std::string_view display_name = "entry")
    {
        yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto          obj = *request.as_object();
        obj[std::string_view("pluginGuid")] =
            std::string_view("00000000-0000-0000-0000-000000000000");
        obj[std::string_view("taskTypeGuid")] =
            std::string_view("A1B2C3D4-E5F6-7890-ABCD-EF1234567890");
        obj[std::string_view("displayName")] = std::string(display_name);
        auto created = scheduler_->CreateRepositoryEntry(request);
        auto created_obj = created.as_object();
        EXPECT_TRUE(created_obj.has_value());
        return (*created_obj)[std::string_view("entryId")].as_sint().value_or(
            -1);
    }

    std::filesystem::path test_dir_;
    std::filesystem::path settings_dir_;
    std::filesystem::path plugin_dir_;
    std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
                                                          settings_manager_;
    DAS::DasPtr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
    std::unique_ptr<Das::Core::ForeignInterfaceHost::TestablePluginManager>
                                                       plugin_manager_;
    std::unique_ptr<SchedulerService>                  scheduler_;
    std::thread                                        io_thread_;
    std::unique_ptr<Das::Http::DasSchedulerController> controller_;
};

// ── Profile guard ──

TEST_F(SchedulerControllerTest, Initialize_ProfileNonZero_Rejected)
{
    auto req =
        MakeRequest("/api/scheduler/1/initialize", "{}", {{"profile", "1"}});
    auto resp = controller_->Initialize(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(scheduler_->IsInitialized());
}

TEST_F(SchedulerControllerTest, Start_ProfileNonZero_Rejected)
{
    auto req = MakeRequest("/api/scheduler/1/start", "{}", {{"profile", "1"}});
    auto resp = controller_->Start(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
}

TEST_F(SchedulerControllerTest, Stop_ProfileNonZero_Rejected)
{
    auto req = MakeRequest("/api/scheduler/1/stop", "{}", {{"profile", "1"}});
    auto resp = controller_->Stop(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
}

TEST_F(SchedulerControllerTest, Get_ProfileNonZero_Rejected)
{
    auto req = MakeRequest("/api/scheduler/1/get", "{}", {{"profile", "1"}});
    auto resp = controller_->Get(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
}

TEST_F(SchedulerControllerTest, AddTask_ProfileNonZero_Rejected)
{
    auto req = MakeRequest(
        "/api/scheduler/1/{taskGuid}/put",
        "{}",
        {{"profile", "1"},
         {"taskGuid", "A1B2C3D4-E5F6-7890-ABCD-EF1234567890"}});
    auto resp = controller_->AddTask(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
}

TEST_F(SchedulerControllerTest, DeleteTask_ProfileNonZero_Rejected)
{
    auto req = MakeRequest(
        "/api/scheduler/1/0/delete",
        "{}",
        {{"profile", "1"}, {"taskId", "0"}});
    auto resp = controller_->DeleteTask(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
}

// ── Malformed GUID ──

TEST_F(SchedulerControllerTest, AddTask_MalformedGuid_Rejected)
{
    auto req = MakeRequest(
        "/api/scheduler/0/not-a-guid/put",
        "{}",
        {{"profile", "0"}, {"taskGuid", "not-a-guid"}});
    auto resp = controller_->AddTask(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
}

// ── Malformed taskId ──

TEST_F(SchedulerControllerTest, DeleteTask_MalformedTaskId_Rejected)
{
    auto req = MakeRequest(
        "/api/scheduler/0/abc/delete",
        "{}",
        {{"profile", "0"}, {"taskId", "abc"}});
    auto resp = controller_->DeleteTask(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
}

TEST_F(SchedulerControllerTest, UpdateProps_MalformedTaskId_Rejected)
{
    auto req = MakeRequest(
        "/api/scheduler/0/xyz/properties/update",
        R"({"key":"val"})",
        {{"profile", "0"}, {"taskId", "xyz"}});
    auto resp = controller_->UpdateTaskProperties(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
}

TEST_F(SchedulerControllerTest, UpdateInternalProps_MalformedTaskId_Rejected)
{
    auto req = MakeRequest(
        "/api/scheduler/0/abc/internal/properties/update",
        R"({"nextExecutionTime":null})",
        {{"profile", "0"}, {"taskId", "abc"}});
    auto resp = controller_->UpdateTaskInternalProperties(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
}

// ── disabledGuids validation ──

TEST_F(SchedulerControllerTest, Initialize_NonArrayDisabledGuids_Rejected)
{
    auto req = MakeRequest(
        "/api/scheduler/0/initialize",
        R"({"disabledGuids":"not-array"})",
        {{"profile", "0"}});
    auto resp = controller_->Initialize(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(scheduler_->IsInitialized());
}

TEST_F(SchedulerControllerTest, Initialize_MalformedGuidMember_Skipped)
{
    auto req = MakeRequest(
        "/api/scheduler/0/initialize",
        R"({"disabledGuids":["not-a-guid","00000000-0000-0000-0000-000000000000"]})",
        {{"profile", "0"}});
    auto resp = controller_->Initialize(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    // Should succeed (malformed GUIDs are skipped, valid ones pass)
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
    EXPECT_TRUE(scheduler_->IsInitialized());
}

// ── Valid request forwarding ──

TEST_F(SchedulerControllerTest, Initialize_AbsentDisabledGuids_Forwarded)
{
    auto req =
        MakeRequest("/api/scheduler/0/initialize", "{}", {{"profile", "0"}});
    auto resp = controller_->Initialize(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
    EXPECT_TRUE(scheduler_->IsInitialized());
}

TEST_F(SchedulerControllerTest, Initialize_Valid_Forwarded)
{
    auto req = MakeRequest(
        "/api/scheduler/0/initialize",
        R"({"disabledGuids":[]})",
        {{"profile", "0"}});
    auto resp = controller_->Initialize(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
    EXPECT_TRUE(scheduler_->IsInitialized());
}

TEST_F(SchedulerControllerTest, Start_Valid_Forwarded)
{
    SeedInitializedWithTask();
    auto req = MakeRequest("/api/scheduler/0/start", "{}", {{"profile", "0"}});
    auto resp = controller_->Start(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
    // 清场：停机，避免夹具析构时存在在飞 tick
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
}

TEST_F(SchedulerControllerTest, Stop_Valid_Forwarded)
{
    SeedInitializedWithTask();
    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    auto req = MakeRequest("/api/scheduler/0/stop", "{}", {{"profile", "0"}});
    auto resp = controller_->Stop(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
}

TEST_F(SchedulerControllerTest, Get_Valid_Forwarded)
{
    SeedInitializedWithTask();
    auto req = MakeRequest("/api/scheduler/0/get", "{}", {{"profile", "0"}});
    auto resp = controller_->Get(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
    EXPECT_TRUE(
        (*body.as_object())[std::string_view("data")].as_object()->contains(
            std::string_view("state")));
}

TEST_F(SchedulerControllerTest, AddTask_Valid_Forwarded)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/scheduler/0/A1B2C3D4-E5F6-7890-ABCD-EF1234567890/put",
        "{}",
        {{"profile", "0"},
         {"taskGuid", "A1B2C3D4-E5F6-7890-ABCD-EF1234567890"}});
    auto resp = controller_->AddTask(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
    // 种入实例 0 占首个 id，调度器索引 nextTaskId 分配下一个实例 id=1
    EXPECT_EQ(
        (*(*body.as_object())[std::string_view("data")]
              .as_object())[std::string_view("taskId")]
            .as_sint()
            .value(),
        1);
}

TEST_F(SchedulerControllerTest, DeleteTask_Valid_Forwarded)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/scheduler/0/0/delete",
        "{}",
        {{"profile", "0"}, {"taskId", "0"}});
    auto resp = controller_->DeleteTask(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
}

// 真实链属性更新：FakeTask 注册无 descriptor 清单，任意属性键都不在
// 描述符表内——真实校验返回 DAS_E_NOT_FOUND，控制器包装为错误封套
// （code 原样透传）。
TEST_F(SchedulerControllerTest, UpdateProps_Valid_Forwarded)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/scheduler/0/0/properties/update",
        R"({"key":"value"})",
        {{"profile", "0"}, {"taskId", "0"}});
    auto resp = controller_->UpdateTaskProperties(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_E_NOT_FOUND);
}

TEST_F(SchedulerControllerTest, UpdateInternalProps_Valid_Forwarded)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/scheduler/0/0/internal/properties/update",
        R"({"nextExecutionTime":1777593600})",
        {{"profile", "0"}, {"taskId", "0"}});
    auto resp = controller_->UpdateTaskInternalProperties(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_OK);
}

// 任务 authoring 端点（真实调度器）：FakeTask 无 authoring capability，
// 真实链返回 errorKind=capabilityMissing——控制器把服务结果原文透传为
// code=DAS_E_FAIL + data 原样（errorKind 封套即翻译契约）。成功封套形状
// 由仓库族（Get/Rename）与 Get/AddTask 用例背书（同一 Dispatch 模板）。
TEST_F(SchedulerControllerTest, SchedulerControllerAuthoringGet_Forwarded)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/v1/scheduler/0/tasks/0/authoring/get",
        R"({"view":"default"})",
        {{"profile", "0"}, {"taskId", "0"}});
    auto resp = controller_->AuthoringGet(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    auto root = body.as_object().value();
    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_S_OK),
        DAS_E_FAIL);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(
        (*data)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("capabilityMissing"));
}

TEST_F(SchedulerControllerTest, SchedulerControllerAuthoringApply_Forwarded)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/v1/scheduler/0/tasks/0/authoring/apply",
        R"({"baseRevision":0,"kind":"setValue","payload":{}})",
        {{"profile", "0"}, {"taskId", "0"}});
    auto resp = controller_->AuthoringApply(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    auto root = body.as_object().value();
    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_S_OK),
        DAS_E_FAIL);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(
        (*data)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("capabilityMissing"));
}

TEST_F(SchedulerControllerTest, SchedulerControllerAuthoringCompile_Forwarded)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/v1/scheduler/0/tasks/0/authoring/compile",
        R"({"mode":"preview"})",
        {{"profile", "0"}, {"taskId", "0"}});
    auto resp = controller_->AuthoringCompile(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    auto root = body.as_object().value();
    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_S_OK),
        DAS_E_FAIL);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(
        (*data)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("capabilityMissing"));
}

// errorKind 数据透传（真实链）：对不存在的任务 apply，真实调度器返回
// errorKind=taskNotFound 封套——控制器翻译为 code=DAS_E_FAIL 且 data
// 原样透传（message/errorKind 字段直达客户端）。
TEST_F(SchedulerControllerTest, SchedulerControllerAuthoringErrorKind_Data)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/v1/scheduler/0/tasks/42/authoring/apply",
        R"({"baseRevision":0,"kind":"setValue","payload":{}})",
        {{"profile", "0"}, {"taskId", "42"}});
    auto resp = controller_->AuthoringApply(req);
    auto body = *Das::Utils::ParseYyjsonFromString(resp.Release().body());
    auto root = body.as_object().value();
    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_S_OK),
        DAS_E_FAIL);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(
        (*data)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("notFound"));
    EXPECT_TRUE(data->contains(std::string_view("message")));
}

TEST_F(SchedulerControllerTest, SchedulerControllerRepositoryGet_Forwarded)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/get",
        "{}",
        {{"profile", "0"}});
    auto body = ReleaseJson(controller_->RepositoryGet(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_E_FAIL),
        DAS_S_OK);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_TRUE(data->contains(std::string_view("entries")));
}

// 真实链 create：FakeTask 注册表内的任务类型（null pluginGuid + FakeTask
// guid）建条目成功，控制器透传分配的 entryId。
TEST_F(SchedulerControllerTest, SchedulerControllerRepositoryCreate_Forwarded)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries",
        R"({"pluginGuid":"00000000-0000-0000-0000-000000000000","taskTypeGuid":"A1B2C3D4-E5F6-7890-ABCD-EF1234567890"})",
        {{"profile", "0"}});
    auto body = ReleaseJson(controller_->RepositoryCreate(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_E_FAIL),
        DAS_S_OK);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_TRUE((*data)[std::string_view("entryId")].as_sint().has_value());
}

TEST_F(SchedulerControllerTest, SchedulerControllerRepositoryProfile_Rejected)
{
    auto get_req = MakeRequest(
        "/api/v1/scheduler/1/repository/get",
        "{}",
        {{"profile", "1"}});
    auto get_body = ReleaseJson(controller_->RepositoryGet(get_req));
    EXPECT_EQ(
        (*get_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);

    auto create_req = MakeRequest(
        "/api/v1/scheduler/1/repository/entries",
        "{}",
        {{"profile", "1"}});
    auto create_body = ReleaseJson(controller_->RepositoryCreate(create_req));
    EXPECT_EQ(
        (*create_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);
}

TEST_F(SchedulerControllerTest, SchedulerControllerRepositoryBody_InvalidJson)
{
    auto get_req = MakeRequest(
        "/api/v1/scheduler/0/repository/get",
        "{broken",
        {{"profile", "0"}});
    auto get_body = ReleaseJson(controller_->RepositoryGet(get_req));
    EXPECT_EQ(
        (*get_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_JSON);

    auto create_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries",
        "{broken",
        {{"profile", "0"}});
    auto create_body = ReleaseJson(controller_->RepositoryCreate(create_req));
    EXPECT_EQ(
        (*create_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_JSON);
}

TEST_F(SchedulerControllerTest, SchedulerControllerRepositoryBody_NonObject)
{
    auto get_req = MakeRequest(
        "/api/v1/scheduler/0/repository/get",
        "[]",
        {{"profile", "0"}});
    auto get_body = ReleaseJson(controller_->RepositoryGet(get_req));
    EXPECT_EQ(
        (*get_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);

    auto create_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries",
        "[]",
        {{"profile", "0"}});
    auto create_body = ReleaseJson(controller_->RepositoryCreate(create_req));
    EXPECT_EQ(
        (*create_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);
}

TEST_F(SchedulerControllerTest, SchedulerControllerRepositoryDelete_Forwarded)
{
    SeedInitializedWithTask();
    const auto entry_id = SeedRepositoryEntry();
    ASSERT_GE(entry_id, 0);
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/" + std::to_string(entry_id)
            + "/delete",
        "{}",
        {{"profile", "0"}, {"entryId", std::to_string(entry_id)}});
    auto body = ReleaseJson(controller_->RepositoryDelete(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_E_FAIL),
        DAS_S_OK);
}

TEST_F(SchedulerControllerTest, SchedulerControllerRepositoryRename_Forwarded)
{
    SeedInitializedWithTask();
    const auto entry_id = SeedRepositoryEntry();
    ASSERT_GE(entry_id, 0);
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/" + std::to_string(entry_id)
            + "/rename",
        R"({"displayName":"Renamed"})",
        {{"profile", "0"}, {"entryId", std::to_string(entry_id)}});
    auto body = ReleaseJson(controller_->RepositoryRename(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_E_FAIL),
        DAS_S_OK);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(
        (*data)[std::string_view("displayName")].as_string().value_or(""),
        std::string_view("Renamed"));
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryProfileManagement_Rejected)
{
    auto delete_req = MakeRequest(
        "/api/v1/scheduler/1/repository/entries/42/delete",
        "{}",
        {{"profile", "1"}, {"entryId", "42"}});
    auto delete_body = ReleaseJson(controller_->RepositoryDelete(delete_req));
    EXPECT_EQ(
        (*delete_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);

    auto rename_req = MakeRequest(
        "/api/v1/scheduler/1/repository/entries/42/rename",
        R"({"displayName":"Renamed"})",
        {{"profile", "1"}, {"entryId", "42"}});
    auto rename_body = ReleaseJson(controller_->RepositoryRename(rename_req));
    EXPECT_EQ(
        (*rename_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);
}

TEST_F(SchedulerControllerTest, SchedulerControllerRepositoryEntryId_Rejected)
{
    auto delete_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/not-an-id/delete",
        "{}",
        {{"profile", "0"}, {"entryId", "not-an-id"}});
    auto delete_body = ReleaseJson(controller_->RepositoryDelete(delete_req));
    EXPECT_EQ(
        (*delete_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);

    auto rename_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/not-an-id/rename",
        R"({"displayName":"Renamed"})",
        {{"profile", "0"}, {"entryId", "not-an-id"}});
    auto rename_body = ReleaseJson(controller_->RepositoryRename(rename_req));
    EXPECT_EQ(
        (*rename_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryRenameBody_InvalidJson)
{
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/rename",
        "{broken",
        {{"profile", "0"}, {"entryId", "42"}});
    auto body = ReleaseJson(controller_->RepositoryRename(req));
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_JSON);
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryRenameBody_NonObject)
{
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/rename",
        "[]",
        {{"profile", "0"}, {"entryId", "42"}});
    auto body = ReleaseJson(controller_->RepositoryRename(req));
    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);
}

// 服务错误映射（真实链）：对不存在的条目 delete，真实调度器返回
// DAS_E_NOT_FOUND——控制器把它包装为错误封套（code 原样透传）。
TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryDelete_ServiceError)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/424242/delete",
        "{}",
        {{"profile", "0"}, {"entryId", "424242"}});
    auto body = ReleaseJson(controller_->RepositoryDelete(req));

    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_NOT_FOUND);
}

// 服务错误映射（真实链）：对不存在的条目 rename，真实调度器返回
// errorKind 封套——控制器翻译为 code=DAS_E_FAIL + data 原样。
TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryRename_ServiceError)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/424242/rename",
        R"({"displayName":"Renamed"})",
        {{"profile", "0"}, {"entryId", "424242"}});
    auto body = ReleaseJson(controller_->RepositoryRename(req));

    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_FAIL);
    auto data = (*body.as_object())[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_TRUE(data->contains(std::string_view("errorKind")));
}

// 条目 authoring 端点（真实链）：条目绑定的任务类型属未加载插件
// （test-registered 特征无插件清单），真实链在能力解析前先被插件可用性
// 门拒绝——errorKind=pluginUnavailable，控制器透传 errorKind 封套。
TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryAuthoringGet_Forwarded)
{
    SeedInitializedWithTask();
    const auto entry_id = SeedRepositoryEntry();
    ASSERT_GE(entry_id, 0);
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/" + std::to_string(entry_id)
            + "/authoring/get",
        R"({"view":"default"})",
        {{"profile", "0"}, {"entryId", std::to_string(entry_id)}});
    auto body = ReleaseJson(controller_->RepositoryAuthoringGet(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_S_OK),
        DAS_E_FAIL);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(
        (*data)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("pluginUnavailable"));
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryAuthoringApply_Forwarded)
{
    SeedInitializedWithTask();
    const auto entry_id = SeedRepositoryEntry();
    ASSERT_GE(entry_id, 0);
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/" + std::to_string(entry_id)
            + "/authoring/apply",
        R"({"baseRevision":0,"kind":"setValue","payload":{}})",
        {{"profile", "0"}, {"entryId", std::to_string(entry_id)}});
    auto body = ReleaseJson(controller_->RepositoryAuthoringApply(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_S_OK),
        DAS_E_FAIL);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(
        (*data)[std::string_view("errorKind")].as_string().value_or(""),
        std::string_view("pluginUnavailable"));
}

// 真实链 compile：无 authoring capability 的条目同样走 errorKind 封套
// （compiledSnapshot wire 族已整体退役——错误形状下无快照键是构造性的）。
TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryCompile_PreviewOnly)
{
    SeedInitializedWithTask();
    const auto entry_id = SeedRepositoryEntry();
    ASSERT_GE(entry_id, 0);
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/" + std::to_string(entry_id)
            + "/authoring/compile",
        R"({"mode":"preview"})",
        {{"profile", "0"}, {"entryId", std::to_string(entry_id)}});
    auto body = ReleaseJson(controller_->RepositoryAuthoringCompile(req));
    auto root = body.as_object().value();

    EXPECT_EQ(
        root[std::string_view("code")].as_sint().value_or(DAS_S_OK),
        DAS_E_FAIL);
    auto data = root[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_TRUE(data->contains(std::string_view("errorKind")));
    const auto child_snapshot_key = std::string{"childExecution"} + "Snapshot";
    const auto execution_snapshot_key = std::string{"execution"} + "Snapshot";
    EXPECT_FALSE(data->contains(std::string_view(child_snapshot_key)));
    EXPECT_FALSE(data->contains(std::string_view(execution_snapshot_key)));
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryAuthoringProfile_Rejected)
{
    auto get_req = MakeRequest(
        "/api/v1/scheduler/1/repository/entries/42/authoring/get",
        "{}",
        {{"profile", "1"}, {"entryId", "42"}});
    auto get_body = ReleaseJson(controller_->RepositoryAuthoringGet(get_req));
    EXPECT_EQ(
        (*get_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);

    auto apply_req = MakeRequest(
        "/api/v1/scheduler/1/repository/entries/42/authoring/apply",
        "{}",
        {{"profile", "1"}, {"entryId", "42"}});
    auto apply_body =
        ReleaseJson(controller_->RepositoryAuthoringApply(apply_req));
    EXPECT_EQ(
        (*apply_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);

    auto compile_req = MakeRequest(
        "/api/v1/scheduler/1/repository/entries/42/authoring/compile",
        "{}",
        {{"profile", "1"}, {"entryId", "42"}});
    auto compile_body =
        ReleaseJson(controller_->RepositoryAuthoringCompile(compile_req));
    EXPECT_EQ(
        (*compile_body.as_object())[std::string_view("code")]
            .as_sint()
            .value_or(DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryAuthoringEntryId_Rejected)
{
    auto get_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/bad/authoring/get",
        "{}",
        {{"profile", "0"}, {"entryId", "bad"}});
    auto get_body = ReleaseJson(controller_->RepositoryAuthoringGet(get_req));
    EXPECT_EQ(
        (*get_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);

    auto apply_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/bad/authoring/apply",
        "{}",
        {{"profile", "0"}, {"entryId", "bad"}});
    auto apply_body =
        ReleaseJson(controller_->RepositoryAuthoringApply(apply_req));
    EXPECT_EQ(
        (*apply_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);

    auto compile_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/bad/authoring/compile",
        "{}",
        {{"profile", "0"}, {"entryId", "bad"}});
    auto compile_body =
        ReleaseJson(controller_->RepositoryAuthoringCompile(compile_req));
    EXPECT_EQ(
        (*compile_body.as_object())[std::string_view("code")]
            .as_sint()
            .value_or(DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryAuthoringBody_InvalidJson)
{
    auto get_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/authoring/get",
        "{broken",
        {{"profile", "0"}, {"entryId", "42"}});
    auto get_body = ReleaseJson(controller_->RepositoryAuthoringGet(get_req));
    EXPECT_EQ(
        (*get_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_JSON);

    auto apply_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/authoring/apply",
        "{broken",
        {{"profile", "0"}, {"entryId", "42"}});
    auto apply_body =
        ReleaseJson(controller_->RepositoryAuthoringApply(apply_req));
    EXPECT_EQ(
        (*apply_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_JSON);

    auto compile_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/authoring/compile",
        "{broken",
        {{"profile", "0"}, {"entryId", "42"}});
    auto compile_body =
        ReleaseJson(controller_->RepositoryAuthoringCompile(compile_req));
    EXPECT_EQ(
        (*compile_body.as_object())[std::string_view("code")]
            .as_sint()
            .value_or(DAS_S_OK),
        DAS_E_INVALID_JSON);
}

TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryAuthoringBody_NonObject)
{
    auto get_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/authoring/get",
        "[]",
        {{"profile", "0"}, {"entryId", "42"}});
    auto get_body = ReleaseJson(controller_->RepositoryAuthoringGet(get_req));
    EXPECT_EQ(
        (*get_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);

    auto apply_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/authoring/apply",
        "[]",
        {{"profile", "0"}, {"entryId", "42"}});
    auto apply_body =
        ReleaseJson(controller_->RepositoryAuthoringApply(apply_req));
    EXPECT_EQ(
        (*apply_body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);

    auto compile_req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/42/authoring/compile",
        "[]",
        {{"profile", "0"}, {"entryId", "42"}});
    auto compile_body =
        ReleaseJson(controller_->RepositoryAuthoringCompile(compile_req));
    EXPECT_EQ(
        (*compile_body.as_object())[std::string_view("code")]
            .as_sint()
            .value_or(DAS_S_OK),
        DAS_E_INVALID_ARGUMENT);
}

// 服务错误映射（真实链）：对不存在的条目 compile，真实调度器返回
// errorKind 封套——code=DAS_E_FAIL + data 原样。
TEST_F(
    SchedulerControllerTest,
    SchedulerControllerRepositoryCompile_ServiceError)
{
    SeedInitializedWithTask();
    auto req = MakeRequest(
        "/api/v1/scheduler/0/repository/entries/424242/authoring/compile",
        R"({"mode":"preview"})",
        {{"profile", "0"}, {"entryId", "424242"}});
    auto body = ReleaseJson(controller_->RepositoryAuthoringCompile(req));

    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value_or(
            DAS_S_OK),
        DAS_E_FAIL);
    auto data = (*body.as_object())[std::string_view("data")].as_object();
    ASSERT_TRUE(data.has_value());
    EXPECT_TRUE(data->contains(std::string_view("errorKind")));
}

// ── Non-initialize paths do not load scheduler plugins ──

TEST_F(SchedulerControllerTest, NonInitializePaths_DelegateOnly)
{
    // Start, Stop, Get only delegate to the service.
    // They never call Initialize（真实调度器：调度器保持未初始化态）。

    auto start_req =
        MakeRequest("/api/scheduler/0/start", "{}", {{"profile", "0"}});
    controller_->Start(start_req);

    auto stop_req =
        MakeRequest("/api/scheduler/0/stop", "{}", {{"profile", "0"}});
    controller_->Stop(stop_req);

    auto get_req =
        MakeRequest("/api/scheduler/0/get", "{}", {{"profile", "0"}});
    controller_->Get(get_req);

    EXPECT_FALSE(scheduler_->IsInitialized());
}

TEST(ProfileControllerTest, GetPluginSettings_SFalseReportsEmptyObjectRecovery)
{
    auto test_dir = UniqueTestDir();
    std::filesystem::create_directories(test_dir);

    Das::Core::SettingsManager::SettingsManager settings_manager(test_dir);
    ASSERT_EQ(settings_manager.CreateProfile("0"), DAS_S_OK);

    auto plugin_settings_path =
        test_dir / DAS_PATH_LITERAL("0")
        / Das::Utils::PathFromUtf8(
            std::string(FactoryPluginGuidString) + ".json");
    {
        std::ofstream ofs(plugin_settings_path);
        ofs << "{corrupt";
    }

    Das::Core::SettingsManager::SettingsServiceImpl settings_service(
        settings_manager);
    Das::Http::DasProfileController controller(settings_service);

    auto request = MakeRequest(
        "/api/profile/0/plugin/get",
        "{}",
        {{"pid", "0"}, {"guid", FactoryPluginGuidString}});

    auto response = controller.GetPluginSettings(request);
    auto body = *Das::Utils::ParseYyjsonFromString(response.Release().body());

    EXPECT_EQ(
        (*body.as_object())[std::string_view("code")].as_sint().value(),
        DAS_S_FALSE);
    EXPECT_EQ(
        (*body.as_object())[std::string_view("message")].as_string().value(),
        "Plugin settings were invalid and restored to an empty object");
    EXPECT_TRUE((*body.as_object())[std::string_view("data")].is_object());

    std::filesystem::remove_all(test_dir);
}

// ============================================================
// End-to-end task-instance lifecycle test
// ============================================================

class SchedulerLifecycleTest : public ::testing::Test
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

        settings_manager_ =
            std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContext(false);
        plugin_manager_ = std::make_unique<
            Das::Core::ForeignInterfaceHost::TestablePluginManager>(
            *settings_manager_,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_.Get()));
        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_.Get()));

        settings_manager_->CreateProfile("0");
    }

    void TearDown() override
    {
        scheduler_.reset();
        plugin_manager_.reset();
        settings_manager_.reset();
        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path test_dir_;
    std::filesystem::path settings_dir_;
    std::filesystem::path plugin_dir_;
    std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
                                                          settings_manager_;
    DAS::DasPtr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
    std::unique_ptr<Das::Core::ForeignInterfaceHost::TestablePluginManager>
                                      plugin_manager_;
    std::unique_ptr<SchedulerService> scheduler_;
};

TEST_F(SchedulerLifecycleTest, FullTaskInstanceLifecycle)
{
    // -- Phase 1: Initialize, add, update, verify split files --

    // 1. Create a FakeTask and register it
    auto* fake = new FakeTask();
    fake->AddRef();
    DasGuid fake_plugin_guid{};
    fake_plugin_guid.data1 = 0xAAAA;
    plugin_manager_->RegisterTestFeature(
        Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK,
        fake_plugin_guid,
        static_cast<IDasBase*>(fake));

    // 2. Initialize
    auto init_result = scheduler_->Initialize(plugin_dir_, {});
    ASSERT_EQ(init_result, DAS_S_OK);

    // 3. Get: initial state has empty tasks queue
    auto state = scheduler_->Get();
    EXPECT_EQ(
        (*state.as_object())[std::string_view("state")].as_string().value(),
        "stopped");
    EXPECT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        0u);

    // 4. AddTask (put): create two task instances
    int64_t task_id_0 = -1;
    auto    add_result = scheduler_->AddTask(FakeTaskGuid, &task_id_0);
    ASSERT_EQ(add_result, DAS_S_OK);
    EXPECT_EQ(task_id_0, 0);

    int64_t task_id_1 = -1;
    add_result = scheduler_->AddTask(FakeTaskGuid, &task_id_1);
    ASSERT_EQ(add_result, DAS_S_OK);
    EXPECT_EQ(task_id_1, 1);

    // 5. Assert split files directly:
    //    scheduler.json.nextTaskId increments
    auto scheduler_index = settings_manager_->GetSchedulerIndexJson("0");
    EXPECT_EQ(
        (*scheduler_index.as_object())[std::string_view("nextTaskId")]
            .as_sint()
            .value(),
        2);
    //    scheduler.json.taskOrder[] order is stable
    ASSERT_EQ(
        (*scheduler_index.as_object())[std::string_view("taskOrder")]
            .as_array()
            ->size(),
        2u);
    EXPECT_EQ(
        (*(*scheduler_index.as_object())[std::string_view("taskOrder")]
              .as_array())[0]
            .as_sint()
            .value(),
        0);
    EXPECT_EQ(
        (*(*scheduler_index.as_object())[std::string_view("taskOrder")]
              .as_array())[1]
            .as_sint()
            .value(),
        1);

    //    taskId{taskId}.json is created for each task instance
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskId0.json")));
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskId1.json")));

    //    Verify task instance file content
    auto task0_json = settings_manager_->GetTaskInstanceJson("0", 0);
    EXPECT_EQ(
        (*task0_json.as_object())[std::string_view("id")].as_sint().value(),
        0);
    EXPECT_EQ(
        (*task0_json.as_object())[std::string_view("taskGuid")]
            .as_string()
            .value(),
        "A1B2C3D4-E5F6-7890-ABCD-EF1234567890");

    // 6. Get: verify two task instances in queue
    state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        2u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        0);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[1]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        1);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "available");
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[1]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "available");

    // 7. UpdateTaskInternalProperties on instance 0
    //    (no descriptors from manifest, so UpdateTaskProperties would reject;
    //     internal properties are always writable when not running)
    yyjson::value internal_props(Das::Utils::MakeYyjsonObject());
    // 2026-07-01T09:00:00Z = 1782896400
    (*internal_props.as_object())[std::string_view("nextExecutionTime")] =
        1782896400;
    auto update_result =
        scheduler_->UpdateTaskInternalProperties(0, internal_props);
    EXPECT_EQ(update_result, DAS_S_OK);

    //    Verify updated properties are persisted under the selected instance
    auto task0_updated = settings_manager_->GetTaskInstanceJson("0", 0);
    EXPECT_EQ(
        (*task0_updated.as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        1782896400);

    // -- Phase 2: Start, verify running mutation guard, stop --

    // 8. Enable (start)
    ASSERT_EQ(scheduler_->Enable(), DAS_S_OK);
    EXPECT_EQ(scheduler_->Status(), SchedulerState::Running);

    // 9. Running mutation guard: add/delete/property mutations reject
    int64_t rejected_id = -1;
    EXPECT_EQ(
        scheduler_->AddTask(FakeTaskGuid, &rejected_id),
        DAS_E_TASK_WORKING);
    EXPECT_EQ(scheduler_->DeleteTask(0), DAS_E_TASK_WORKING);

    yyjson::value rejected_props(Das::Utils::MakeYyjsonObject());
    (*rejected_props.as_object())[std::string_view("key")] = "val";
    EXPECT_EQ(
        scheduler_->UpdateTaskProperties(0, rejected_props),
        DAS_E_TASK_WORKING);
    EXPECT_EQ(
        scheduler_->UpdateTaskInternalProperties(0, internal_props),
        DAS_E_TASK_WORKING);

    // 10. Disable (stop) -- clears runtime state but split files remain
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);
    ASSERT_TRUE(
        WaitForSchedulerStopped(*scheduler_, std::chrono::milliseconds(2000)));

    // Split files should still exist after Disable
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskId0.json")));
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskId1.json")));
    auto index_after_stop = settings_manager_->GetSchedulerIndexJson("0");
    EXPECT_EQ(
        (*index_after_stop.as_object())[std::string_view("nextTaskId")]
            .as_sint()
            .value(),
        2);

    // -- Phase 3: Re-initialize, delete, verify persistence across instances --

    // 11. Re-register the fake task (Disable unloaded plugins)
    plugin_manager_->RegisterTestFeature(
        Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK,
        fake_plugin_guid,
        static_cast<IDasBase*>(fake));

    // 12. Re-initialize to re-materialize from persisted split files
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        2u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        0);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[1]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        1);
    // Task 0's nextExecutionTime was persisted
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("nextExecutionTime")]
            .as_sint()
            .value(),
        1782896400);

    // 13. Delete task 0
    auto delete_result = scheduler_->DeleteTask(0);
    ASSERT_EQ(delete_result, DAS_S_OK);

    //    delete removes only the selected taskId{taskId}.json and queue-order
    //    entry
    EXPECT_FALSE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskId0.json")));
    EXPECT_TRUE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskId1.json")));

    auto index_after_delete = settings_manager_->GetSchedulerIndexJson("0");
    ASSERT_EQ(
        (*index_after_delete.as_object())[std::string_view("taskOrder")]
            .as_array()
            ->size(),
        1u);
    EXPECT_EQ(
        (*(*index_after_delete.as_object())[std::string_view("taskOrder")]
              .as_array())[0]
            .as_sint()
            .value(),
        1);

    // 14. Get: only task 1 remains
    state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        1);

    // 15. A new scheduler instance sees persisted final state
    auto ipc_sp2 = DAS::Core::IPC::MainProcess::CreateIpcContext(false);
    auto scheduler2 = std::make_unique<SchedulerService>(
        *plugin_manager_,
        Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(ipc_sp2.Get()));
    ASSERT_EQ(scheduler2->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state2 = scheduler2->Get();
    ASSERT_EQ(
        (*state2.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state2.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        1);
    EXPECT_EQ(
        (*(*(*state2.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("taskGuid")]
            .as_string()
            .value(),
        "A1B2C3D4-E5F6-7890-ABCD-EF1234567890");

    // Cleanup: release the fake task
    fake->Release();
}

TEST_F(
    SchedulerLifecycleTest,
    StaticDescriptorCompatibilityAuthoringCapabilityMissing)
{
    auto* fake = new FakeTask();
    fake->AddRef();
    DasGuid fake_plugin_guid{};
    fake_plugin_guid.data1 = 0xAAAA;
    plugin_manager_->RegisterTestFeature(
        Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK,
        fake_plugin_guid,
        static_cast<IDasBase*>(fake));

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    int64_t task_id = -1;
    ASSERT_EQ(scheduler_->AddTask(FakeTaskGuid, &task_id), DAS_S_OK);

    const yyjson::value request(Das::Utils::MakeYyjsonObject());
    auto document = scheduler_->GetTaskAuthoringDocument(task_id, request);
    ASSERT_TRUE(document.is_object());
    EXPECT_EQ(
        (*document.as_object())[std::string_view("errorKind")]
            .as_string()
            .value_or(""),
        std::string_view("capabilityMissing"));

    yyjson::value change(Das::Utils::MakeYyjsonObject());
    (*change.as_object())[std::string_view("baseRevision")] = 0;
    yyjson::value element(Das::Utils::MakeYyjsonObject());
    (*element.as_object())[std::string_view("kind")] =
        std::make_pair(std::string_view("setValue"), yyjson::copy_string);
    (*element.as_object())[std::string_view("payload")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    auto changes = Das::Utils::MakeYyjsonArray();
    (*changes.as_array()).emplace_back(std::move(element));
    (*change.as_object())[std::string_view("changes")] = std::move(changes);
    auto apply = scheduler_->ApplyTaskAuthoringChange(task_id, change);
    ASSERT_TRUE(apply.is_object());
    EXPECT_EQ(
        (*apply.as_object())[std::string_view("errorKind")]
            .as_string()
            .value_or(""),
        std::string_view("capabilityMissing"));

    auto compile = scheduler_->CompileTaskAuthoring(task_id, request);
    ASSERT_TRUE(compile.is_object());
    EXPECT_EQ(
        (*compile.as_object())[std::string_view("errorKind")]
            .as_string()
            .value_or(""),
        std::string_view("capabilityMissing"));

    auto state = scheduler_->Get();
    ASSERT_TRUE(state.is_object());
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value_or(""),
        std::string_view("available"));

    fake->Release();
}

// ============================================================
// Unavailable and corrupt persisted task-instance coverage
// ============================================================

class SchedulerUnavailableTest : public ::testing::Test
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

        settings_manager_ =
            std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContext(false);
        plugin_manager_ = std::make_unique<
            Das::Core::ForeignInterfaceHost::TestablePluginManager>(
            *settings_manager_,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_.Get()));
        scheduler_ = std::make_unique<SchedulerService>(
            *plugin_manager_,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_.Get()));

        settings_manager_->CreateProfile("0");
    }

    void TearDown() override
    {
        scheduler_.reset();
        plugin_manager_.reset();
        settings_manager_.reset();
        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path test_dir_;
    std::filesystem::path settings_dir_;
    std::filesystem::path plugin_dir_;
    std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
                                                          settings_manager_;
    DAS::DasPtr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
    std::unique_ptr<Das::Core::ForeignInterfaceHost::TestablePluginManager>
                                      plugin_manager_;
    std::unique_ptr<SchedulerService> scheduler_;
};

TEST_F(SchedulerUnavailableTest, UnavailableAndCorruptInstances_VisibleInGet)
{
    // Seed scheduler.json with two task ids: one referencing an absent task
    // type GUID (unavailable), one with a corrupt JSON file (invalid).
    yyjson::value scheduler_index(Das::Utils::MakeYyjsonObject());
    (*scheduler_index.as_object())[std::string_view("nextTaskId")] = 2;
    {
        yyjson::value arr(Das::Utils::MakeYyjsonArray());
        (*arr.as_array()).emplace_back(0);
        (*arr.as_array()).emplace_back(1);
        (*scheduler_index.as_object())[std::string_view("taskOrder")] =
            std::move(arr);
    }
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    // Task 0: unavailable -- references a GUID not in loaded manifests
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-2222-3333-4444-555555555555";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task0.as_object())[std::string_view("nextExecutionTime")] =
        yyjson::value{};
    {
        yyjson::value p(Das::Utils::MakeYyjsonObject());
        (*p.as_object())[std::string_view("customProp")] = "preservedValue";
        (*task0.as_object())[std::string_view("properties")] = std::move(p);
    }
    settings_manager_->UpdateTaskInstanceJson("0", 0, task0);

    // Task 1: corrupt task-instance JSON file
    auto task1_file = settings_dir_ / DAS_PATH_LITERAL("0")
                      / DAS_PATH_LITERAL("taskId1.json");
    {
        std::ofstream ofs{task1_file};
        ofs << "CORRUPT DATA {{{{";
    }

    // Initialize and assert
    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        2u);

    // Unavailable instance: visible with original id/GUID/properties
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        0);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "unavailable");
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("taskGuid")]
            .as_string()
            .value(),
        "11111111-2222-3333-4444-555555555555");
    EXPECT_EQ(
        (*(*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
                .as_object())[std::string_view("properties")]
              .as_object())[std::string_view("customProp")]
            .as_string()
            .value(),
        "preservedValue");

    // Invalid/corrupt instance: visible with original id
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[1]
              .as_object())[std::string_view("id")]
            .as_sint()
            .value(),
        1);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[1]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "invalid");
    auto reason =
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[1]
              .as_object())[std::string_view("unavailabilityReason")]
            .as_string();
    ASSERT_TRUE(reason.has_value());
    EXPECT_FALSE(reason->empty());
}

TEST_F(SchedulerUnavailableTest, UnavailableInstance_NotExecutedOnStart)
{
    // Seed scheduler with an unavailable task type
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-2222-3333-4444-555555555555";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Enable should fail because no available task instances
    auto result = scheduler_->Enable();
    EXPECT_EQ(result, DAS_E_OBJECT_NOT_INIT);
    EXPECT_EQ(scheduler_->Status(), SchedulerState::Stopped);
}

TEST_F(SchedulerUnavailableTest, CorruptInstance_NotExecutedOnStart)
{
    // Seed scheduler with a corrupt task file
    yyjson::value scheduler_index(Das::Utils::MakeYyjsonObject());
    (*scheduler_index.as_object())[std::string_view("nextTaskId")] = 1;
    {
        yyjson::value arr(Das::Utils::MakeYyjsonArray());
        (*arr.as_array()).emplace_back(0);
        (*scheduler_index.as_object())[std::string_view("taskOrder")] =
            std::move(arr);
    }
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    auto task_file = settings_dir_ / DAS_PATH_LITERAL("0")
                     / DAS_PATH_LITERAL("taskId0.json");
    {
        std::ofstream ofs{task_file};
        ofs << "CORRUPT";
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    // Enable should fail because no available task instances
    auto result = scheduler_->Enable();
    EXPECT_EQ(result, DAS_E_OBJECT_NOT_INIT);
    EXPECT_EQ(scheduler_->Status(), SchedulerState::Stopped);
}

TEST_F(SchedulerUnavailableTest, DeleteUnavailableInstance_Succeeds)
{
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-2222-3333-4444-555555555555";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());
    WriteSchedulerState(*settings_manager_, 1, {0}, {task0});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "unavailable");

    auto result = scheduler_->DeleteTask(0);
    EXPECT_EQ(result, DAS_S_OK);

    state = scheduler_->Get();
    EXPECT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        0u);

    // Verify persisted removal
    EXPECT_FALSE(
        std::filesystem::exists(
            settings_dir_ / DAS_PATH_LITERAL("0")
            / DAS_PATH_LITERAL("taskId0.json")));
    auto index = settings_manager_->GetSchedulerIndexJson("0");
    EXPECT_EQ(
        (*index.as_object())[std::string_view("taskOrder")].as_array()->size(),
        0u);
}

TEST_F(SchedulerUnavailableTest, DeleteCorruptInstance_Succeeds)
{
    yyjson::value scheduler_index(Das::Utils::MakeYyjsonObject());
    (*scheduler_index.as_object())[std::string_view("nextTaskId")] = 1;
    {
        yyjson::value arr(Das::Utils::MakeYyjsonArray());
        (*arr.as_array()).emplace_back(0);
        (*scheduler_index.as_object())[std::string_view("taskOrder")] =
            std::move(arr);
    }
    settings_manager_->UpdateSchedulerIndexJson("0", scheduler_index);

    auto task_file = settings_dir_ / DAS_PATH_LITERAL("0")
                     / DAS_PATH_LITERAL("taskId0.json");
    {
        std::ofstream ofs{task_file};
        ofs << "corrupt";
    }

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        1u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "invalid");

    auto result = scheduler_->DeleteTask(0);
    EXPECT_EQ(result, DAS_S_OK);

    state = scheduler_->Get();
    EXPECT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        0u);
}

TEST_F(SchedulerUnavailableTest, MixedAvailableAndUnavailable_OnlyAvailableRuns)
{
    // Start IO thread so steady_timer can fire for tick execution
    auto&       io = ipc_sp_->GetIoContext();
    std::thread io_thread(
        [&io]()
        {
            const boost::asio::executor_work_guard<
                boost::asio::io_context::executor_type>
                work(io.get_executor());
            io.run();
        });

    // Register a fake available task
    auto* fake = new FakeTask();
    fake->AddRef();
    DasGuid fake_plugin_guid{};
    fake_plugin_guid.data1 = 0xAAAA;
    plugin_manager_->RegisterTestFeature(
        Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK,
        fake_plugin_guid,
        static_cast<IDasBase*>(fake));

    // Task 0: unavailable (GUID not matching any loaded task)
    yyjson::value task0(Das::Utils::MakeYyjsonObject());
    (*task0.as_object())[std::string_view("id")] = 0;
    (*task0.as_object())[std::string_view("taskGuid")] =
        "11111111-2222-3333-4444-555555555555";
    (*task0.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000002";
    (*task0.as_object())[std::string_view("properties")] =
        yyjson::value(Das::Utils::MakeYyjsonObject());

    // Task 1: available (matches FakeTaskGuid)
    yyjson::value task1(Das::Utils::MakeYyjsonObject());
    (*task1.as_object())[std::string_view("id")] = 1;
    (*task1.as_object())[std::string_view("taskGuid")] =
        "A1B2C3D4-E5F6-7890-ABCD-EF1234567890";
    (*task1.as_object())[std::string_view("pluginGuid")] =
        "00000000-0000-0000-0000-000000000000";
    {
        yyjson::value p(Das::Utils::MakeYyjsonObject());
        (*p.as_object())[std::string_view("testKey")] = "testValue";
        (*task1.as_object())[std::string_view("properties")] = std::move(p);
    }

    WriteSchedulerState(*settings_manager_, 2, {0, 1}, {task0, task1});

    ASSERT_EQ(scheduler_->Initialize(plugin_dir_, {}), DAS_S_OK);

    auto state = scheduler_->Get();
    ASSERT_EQ(
        (*state.as_object())[std::string_view("tasks")].as_array()->size(),
        2u);
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[0]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "unavailable");
    EXPECT_EQ(
        (*(*(*state.as_object())[std::string_view("tasks")].as_array())[1]
              .as_object())[std::string_view("availability")]
            .as_string()
            .value(),
        "available");

    // Enable should succeed because at least one available instance exists
    EXPECT_EQ(scheduler_->Enable(), DAS_S_OK);

    // Give the scheduler time to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Disable
    ASSERT_EQ(scheduler_->Disable(), DAS_S_OK);

    // The fake task should have been executed (not the unavailable one)
    EXPECT_GE(fake->do_call_count, 1);

    io.stop();
    if (io_thread.joinable())
    {
        io_thread.join();
    }

    fake->Release();
}
